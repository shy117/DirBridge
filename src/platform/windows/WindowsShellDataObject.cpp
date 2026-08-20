#include "platform/windows/WindowsShellDataObject.h"

#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <shldisp.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
class ShellDataObject;
std::mutex g_clipboardMutex;
ShellDataObject *g_clipboardObject = nullptr;

std::function<void()> onceCallback(std::function<void()> callback)
{
    auto called = std::make_shared<std::atomic_bool>(false);
    return [called, callback = std::move(callback)]() {
        if (!called->exchange(true) && callback)
        {
            callback();
        }
    };
}

class FormatEnumerator final : public IEnumFORMATETC
{
public:
    explicit FormatEnumerator(std::vector<FORMATETC> formats)
        : m_formats(std::move(formats))
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override
    {
        if (object == nullptr)
        {
            return E_POINTER;
        }
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IEnumFORMATETC)
        {
            *object = static_cast<IEnumFORMATETC *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG references = --m_references;
        if (references == 0)
        {
            delete this;
        }
        return references;
    }

    HRESULT STDMETHODCALLTYPE Next(ULONG count, FORMATETC *formats, ULONG *fetched) override
    {
        if (formats == nullptr || (fetched == nullptr && count != 1))
        {
            return E_POINTER;
        }
        ULONG copied = 0;
        while (copied < count && m_index < m_formats.size())
        {
            formats[copied] = m_formats[m_index];
            ++copied;
            ++m_index;
        }
        if (fetched != nullptr)
        {
            *fetched = copied;
        }
        return copied == count ? S_OK : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE Skip(ULONG count) override
    {
        m_index = std::min(m_index + static_cast<std::size_t>(count), m_formats.size());
        return m_index == m_formats.size() ? S_FALSE : S_OK;
    }

    HRESULT STDMETHODCALLTYPE Reset() override
    {
        m_index = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC **enumerator) override
    {
        if (enumerator == nullptr)
        {
            return E_POINTER;
        }
        auto *copy = new FormatEnumerator(m_formats);
        copy->m_index = m_index;
        *enumerator = copy;
        return S_OK;
    }

private:
    std::atomic<ULONG> m_references{1};
    std::vector<FORMATETC> m_formats;
    std::size_t m_index = 0;
};

class AsyncContentState final
{
public:
    ~AsyncContentState()
    {
        if (!temporaryPath.empty())
        {
            DeleteFileW(temporaryPath.c_str());
        }
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::wstring temporaryPath;
    std::shared_ptr<std::atomic_bool> canceled = std::make_shared<std::atomic_bool>(false);
    bool done = false;
    bool success = false;
};

class DeferredFileStream final : public IStream
{
public:
    explicit DeferredFileStream(std::shared_ptr<AsyncContentState> state)
        : m_state(std::move(state))
    {
    }

    ~DeferredFileStream()
    {
        m_state->canceled->store(true);
        if (m_inner != nullptr)
        {
            m_inner->Release();
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override
    {
        if (object == nullptr)
        {
            return E_POINTER;
        }
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_ISequentialStream || iid == IID_IStream)
        {
            *object = static_cast<IStream *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG references = --m_references;
        if (references == 0)
        {
            delete this;
        }
        return references;
    }

    HRESULT STDMETHODCALLTYPE Read(void *buffer, ULONG count, ULONG *read) override
    {
        const HRESULT result = ensureInner();
        return FAILED(result) ? result : m_inner->Read(buffer, count, read);
    }
    HRESULT STDMETHODCALLTYPE Write(const void *buffer, ULONG count, ULONG *written) override
    {
        return STG_E_ACCESSDENIED;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *position) override
    {
        const HRESULT result = ensureInner();
        return FAILED(result) ? result : m_inner->Seek(move, origin, position);
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override { return STG_E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream *destination, ULARGE_INTEGER count, ULARGE_INTEGER *read, ULARGE_INTEGER *written) override
    {
        const HRESULT result = ensureInner();
        return FAILED(result) ? result : m_inner->CopyTo(destination, count, read, written);
    }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return STG_E_REVERTED; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG *stat, DWORD flags) override
    {
        const HRESULT result = ensureInner();
        return FAILED(result) ? result : m_inner->Stat(stat, flags);
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream **stream) override
    {
        if (stream == nullptr)
        {
            return E_POINTER;
        }
        *stream = nullptr;
        const HRESULT result = ensureInner();
        return FAILED(result) ? result : m_inner->Clone(stream);
    }

private:
    HRESULT ensureInner()
    {
        std::unique_lock<std::mutex> lock(m_state->mutex);
        m_state->condition.wait(lock, [this]() { return m_state->done; });
        if (!m_state->success || m_state->temporaryPath.empty())
        {
            return STG_E_READFAULT;
        }
        if (m_inner != nullptr)
        {
            return S_OK;
        }
        return SHCreateStreamOnFileEx(
            m_state->temporaryPath.c_str(),
            STGM_READ | STGM_SHARE_DENY_NONE,
            FILE_ATTRIBUTE_NORMAL,
            FALSE,
            nullptr,
            &m_inner);
    }

    std::atomic<ULONG> m_references{1};
    std::shared_ptr<AsyncContentState> m_state;
    IStream *m_inner = nullptr;
};

class ShellDataObject final : public IDataObject, public IDataObjectAsyncCapability
{
public:
    ShellDataObject(
        std::vector<WindowsShellDragFile> files,
        QByteArray applicationMetadata = {},
        std::function<void()> onDataObjectReleased = {})
        : ShellDataObject(
              std::move(files),
              WindowsShellDragFileProvider{},
              std::move(applicationMetadata),
              std::move(onDataObjectReleased))
    {
    }

    ShellDataObject(
        WindowsShellDragFileProvider fileProvider,
        QByteArray applicationMetadata,
        std::function<void()> onDataObjectReleased = {})
        : ShellDataObject(
              {},
              std::move(fileProvider),
              std::move(applicationMetadata),
              std::move(onDataObjectReleased))
    {
    }

private:
    enum class FilePreparationState
    {
        Unprepared,
        Preparing,
        Prepared,
        Failed
    };

    ShellDataObject(
        std::vector<WindowsShellDragFile> files,
        WindowsShellDragFileProvider fileProvider,
        QByteArray applicationMetadata,
        std::function<void()> onDataObjectReleased)
        : m_files(std::move(files)),
          m_fileProvider(std::move(fileProvider)),
          m_filePreparationState(m_fileProvider ? FilePreparationState::Unprepared : FilePreparationState::Prepared),
          m_applicationMetadata(std::move(applicationMetadata)),
          m_onDataObjectReleased(std::move(onDataObjectReleased))
    {
        CoCreateFreeThreadedMarshaler(
            static_cast<IUnknown *>(static_cast<IDataObject *>(this)),
            &m_freeThreadedMarshaler);

        m_descriptorFormat.cfFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));
        m_descriptorFormat.ptd = nullptr;
        m_descriptorFormat.dwAspect = DVASPECT_CONTENT;
        m_descriptorFormat.lindex = -1;
        m_descriptorFormat.tymed = TYMED_HGLOBAL;

        m_contentsFormat.cfFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS));
        m_contentsFormat.ptd = nullptr;
        m_contentsFormat.dwAspect = DVASPECT_CONTENT;
        m_contentsFormat.lindex = 0;
        m_contentsFormat.tymed = TYMED_ISTREAM;

        m_effectFormat.cfFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
        m_effectFormat.ptd = nullptr;
        m_effectFormat.dwAspect = DVASPECT_CONTENT;
        m_effectFormat.lindex = -1;
        m_effectFormat.tymed = TYMED_HGLOBAL;

        m_applicationMetadataFormat.cfFormat = static_cast<CLIPFORMAT>(
            RegisterClipboardFormatW(L"application/x-dirbridge-remote-paths"));
        m_applicationMetadataFormat.ptd = nullptr;
        m_applicationMetadataFormat.dwAspect = DVASPECT_CONTENT;
        m_applicationMetadataFormat.lindex = -1;
        m_applicationMetadataFormat.tymed = TYMED_HGLOBAL;
    }

public:
    ~ShellDataObject()
    {
        {
            std::lock_guard<std::mutex> lock(g_clipboardMutex);
            if (g_clipboardObject == this)
            {
                g_clipboardObject = nullptr;
            }
        }
        if (m_freeThreadedMarshaler != nullptr)
        {
            m_freeThreadedMarshaler->Release();
            m_freeThreadedMarshaler = nullptr;
        }
        if (m_onDataObjectReleased)
        {
            m_onDataObjectReleased();
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override
    {
        if (object == nullptr)
        {
            return E_POINTER;
        }
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDataObject)
        {
            *object = static_cast<IDataObject *>(this);
            AddRef();
            return S_OK;
        }
        if (iid == IID_IDataObjectAsyncCapability)
        {
            *object = static_cast<IDataObjectAsyncCapability *>(this);
            AddRef();
            return S_OK;
        }
        if (iid == IID_IMarshal && m_freeThreadedMarshaler != nullptr)
        {
            return m_freeThreadedMarshaler->QueryInterface(iid, object);
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG references = --m_references;
        if (references == 0)
        {
            delete this;
        }
        return references;
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC *format, STGMEDIUM *medium) override
    {
        if (format == nullptr || medium == nullptr)
        {
            return E_POINTER;
        }
        ZeroMemory(medium, sizeof(*medium));
        if (format->cfFormat == m_descriptorFormat.cfFormat && (format->tymed & TYMED_HGLOBAL) != 0)
        {
            if (!ensureFilesPrepared())
            {
                return E_FAIL;
            }
            return createDescriptor(medium);
        }
        if (format->cfFormat == m_effectFormat.cfFormat && (format->tymed & TYMED_HGLOBAL) != 0)
        {
            return createPreferredEffect(medium);
        }
        if (format->cfFormat == m_applicationMetadataFormat.cfFormat
            && (format->tymed & TYMED_HGLOBAL) != 0
            && !m_applicationMetadata.isEmpty())
        {
            return createApplicationMetadata(medium);
        }
        if (format->cfFormat == m_contentsFormat.cfFormat && (format->tymed & TYMED_ISTREAM) != 0)
        {
            if (!ensureFilesPrepared())
            {
                return E_FAIL;
            }
            if (format->lindex < 0 || static_cast<std::size_t>(format->lindex) >= m_files.size())
            {
                return DV_E_LINDEX;
            }
            if (m_files[static_cast<std::size_t>(format->lindex)].isDirectory)
            {
                return DV_E_FORMATETC;
            }
            return createContents(static_cast<std::size_t>(format->lindex), medium);
        }
        return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override { return DATA_E_FORMATETC; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *format) override
    {
        if (format == nullptr)
        {
            return E_POINTER;
        }
        if (format->cfFormat == m_descriptorFormat.cfFormat && (format->tymed & TYMED_HGLOBAL) != 0)
        {
            return hasPotentialFiles() ? S_OK : DV_E_FORMATETC;
        }
        if (format->cfFormat == m_effectFormat.cfFormat && (format->tymed & TYMED_HGLOBAL) != 0)
        {
            return S_OK;
        }
        if (format->cfFormat == m_applicationMetadataFormat.cfFormat
            && (format->tymed & TYMED_HGLOBAL) != 0
            && !m_applicationMetadata.isEmpty())
        {
            return S_OK;
        }
        if (format->cfFormat == m_contentsFormat.cfFormat && (format->tymed & TYMED_ISTREAM) != 0
            && canProvideContents(format->lindex))
        {
            return S_OK;
        }
        return DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *result) override
    {
        if (result != nullptr)
        {
            result->ptd = nullptr;
        }
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC **enumerator) override
    {
        if (enumerator == nullptr)
        {
            return E_POINTER;
        }
        *enumerator = nullptr;
        if (direction != DATADIR_GET)
        {
            return E_NOTIMPL;
        }
        std::vector<FORMATETC> formats{m_descriptorFormat, m_contentsFormat, m_effectFormat};
        if (!m_applicationMetadata.isEmpty())
        {
            formats.push_back(m_applicationMetadataFormat);
        }
        *enumerator = new FormatEnumerator(std::move(formats));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override { return OLE_E_ADVISENOTSUPPORTED; }

    HRESULT STDMETHODCALLTYPE SetAsyncMode(WINBOOL async) override
    {
        m_asyncMode.store(async != FALSE);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetAsyncMode(WINBOOL *async) override
    {
        if (async == nullptr)
        {
            return E_POINTER;
        }
        *async = m_asyncMode.load() ? TRUE : FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE StartOperation(IBindCtx *) override
    {
        m_asyncOperationRunning.store(true);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE InOperation(WINBOOL *running) override
    {
        if (running == nullptr)
        {
            return E_POINTER;
        }
        *running = m_asyncOperationRunning.load() ? TRUE : FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EndOperation(HRESULT, IBindCtx *, DWORD) override
    {
        m_asyncOperationRunning.store(false);
        return S_OK;
    }

private:
    bool ensureFilesPrepared()
    {
        WindowsShellDragFileProvider provider;
        {
            std::lock_guard<std::mutex> lock(m_filePreparationMutex);
            if (m_filePreparationState == FilePreparationState::Prepared)
            {
                return !m_files.empty();
            }
            if (m_filePreparationState == FilePreparationState::Preparing
                || m_filePreparationState == FilePreparationState::Failed
                || !m_fileProvider)
            {
                return false;
            }
            m_filePreparationState = FilePreparationState::Preparing;
            provider = m_fileProvider;
        }

        std::vector<WindowsShellDragFile> preparedFiles;
        bool success = false;
        try
        {
            success = provider(preparedFiles) && !preparedFiles.empty();
        }
        catch (...)
        {
            success = false;
        }

        {
            std::lock_guard<std::mutex> lock(m_filePreparationMutex);
            if (success)
            {
                m_files = std::move(preparedFiles);
                m_filePreparationState = FilePreparationState::Prepared;
            }
            else
            {
                m_filePreparationState = FilePreparationState::Failed;
            }
            m_fileProvider = {};
        }
        return success;
    }

    bool hasPotentialFiles()
    {
        std::lock_guard<std::mutex> lock(m_filePreparationMutex);
        return m_filePreparationState == FilePreparationState::Unprepared
            || m_filePreparationState == FilePreparationState::Preparing
            || (m_filePreparationState == FilePreparationState::Prepared && !m_files.empty());
    }

    bool canProvideContents(LONG index)
    {
        if (index < 0)
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(m_filePreparationMutex);
        if (m_filePreparationState == FilePreparationState::Unprepared
            || m_filePreparationState == FilePreparationState::Preparing)
        {
            return true;
        }
        return m_filePreparationState == FilePreparationState::Prepared
            && static_cast<std::size_t>(index) < m_files.size()
            && !m_files[static_cast<std::size_t>(index)].isDirectory;
    }

    HRESULT createDescriptor(STGMEDIUM *medium)
    {
        const SIZE_T size = sizeof(FILEGROUPDESCRIPTORW) + (m_files.size() - 1) * sizeof(FILEDESCRIPTORW);
        HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
        if (global == nullptr)
        {
            return E_OUTOFMEMORY;
        }
        auto *descriptor = static_cast<FILEGROUPDESCRIPTORW *>(GlobalLock(global));
        if (descriptor == nullptr)
        {
            GlobalFree(global);
            return E_OUTOFMEMORY;
        }
        descriptor->cItems = static_cast<UINT>(m_files.size());
        for (std::size_t index = 0; index < m_files.size(); ++index)
        {
            FILEDESCRIPTORW &file = descriptor->fgd[index];
            file.dwFlags = FD_ATTRIBUTES;
            file.dwFileAttributes = m_files[index].isDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            if (!m_files[index].isDirectory)
            {
                file.dwFlags |= FD_FILESIZE;
                file.nFileSizeHigh = static_cast<DWORD>(m_files[index].size >> 32U);
                file.nFileSizeLow = static_cast<DWORD>(m_files[index].size & 0xffffffffULL);
            }
            wcsncpy_s(file.cFileName, ARRAYSIZE(file.cFileName), m_files[index].fileName.c_str(), _TRUNCATE);
        }
        GlobalUnlock(global);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = global;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT createApplicationMetadata(STGMEDIUM *medium)
    {
        HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(m_applicationMetadata.size()));
        if (global == nullptr)
        {
            return E_OUTOFMEMORY;
        }
        void *buffer = GlobalLock(global);
        if (buffer == nullptr)
        {
            GlobalFree(global);
            return E_OUTOFMEMORY;
        }
        std::memcpy(buffer, m_applicationMetadata.constData(), static_cast<std::size_t>(m_applicationMetadata.size()));
        GlobalUnlock(global);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = global;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT createPreferredEffect(STGMEDIUM *medium)
    {
        HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
        if (global == nullptr)
        {
            return E_OUTOFMEMORY;
        }
        auto *effect = static_cast<DWORD *>(GlobalLock(global));
        if (effect == nullptr)
        {
            GlobalFree(global);
            return E_OUTOFMEMORY;
        }
        *effect = DROPEFFECT_COPY;
        GlobalUnlock(global);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = global;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT createContents(std::size_t index, STGMEDIUM *medium)
    {
        if (!m_files[index].materialize)
        {
            return E_FAIL;
        }
        auto state = std::make_shared<AsyncContentState>();
        {
            std::lock_guard<std::mutex> lock(m_asyncContentsMutex);
            m_asyncContents.push_back(state);
        }
        const auto materialize = m_files[index].materialize;
        try
        {
            std::thread([state, materialize]() {
                std::wstring temporaryPath;
                bool success = false;
                try
                {
                    success = materialize(temporaryPath, state->canceled);
                }
                catch (...)
                {
                    success = false;
                    temporaryPath.clear();
                }
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->temporaryPath = std::move(temporaryPath);
                    state->success = success;
                    state->done = true;
                }
                state->condition.notify_all();
            }).detach();
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(m_asyncContentsMutex);
            const auto stateIt = std::find(m_asyncContents.begin(), m_asyncContents.end(), state);
            if (stateIt != m_asyncContents.end())
            {
                m_asyncContents.erase(stateIt);
            }
            return E_OUTOFMEMORY;
        }
        auto *stream = new DeferredFileStream(std::move(state));
        medium->tymed = TYMED_ISTREAM;
        medium->pstm = stream;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    std::atomic<ULONG> m_references{1};
    std::vector<WindowsShellDragFile> m_files;
    WindowsShellDragFileProvider m_fileProvider;
    FilePreparationState m_filePreparationState = FilePreparationState::Prepared;
    std::mutex m_filePreparationMutex;
    std::vector<std::shared_ptr<AsyncContentState>> m_asyncContents;
    std::mutex m_asyncContentsMutex;
    QByteArray m_applicationMetadata;
    std::function<void()> m_onDataObjectReleased;
    IUnknown *m_freeThreadedMarshaler = nullptr;
    std::atomic_bool m_asyncMode{false};
    std::atomic_bool m_asyncOperationRunning{false};
    FORMATETC m_descriptorFormat{};
    FORMATETC m_contentsFormat{};
    FORMATETC m_effectFormat{};
    FORMATETC m_applicationMetadataFormat{};
};

class ShellDropSource final : public IDropSource
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override
    {
        if (object == nullptr)
        {
            return E_POINTER;
        }
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDropSource)
        {
            *object = static_cast<IDropSource *>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG references = --m_references;
        if (references == 0)
        {
            delete this;
        }
        return references;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) override
    {
        if (escapePressed)
        {
            return DRAGDROP_S_CANCEL;
        }
        if ((keyState & MK_LBUTTON) == 0)
        {
            return DRAGDROP_S_DROP;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    std::atomic<ULONG> m_references{1};
};
} // namespace

bool executeWindowsShellDrag(
    const std::vector<WindowsShellDragFile> &files,
    const QByteArray &applicationMetadata,
    std::function<void()> onDataObjectReleased)
{
    if (files.empty())
    {
        if (onDataObjectReleased)
        {
            onDataObjectReleased();
        }
        return false;
    }
    const HRESULT initResult = OleInitialize(nullptr);
    if (FAILED(initResult))
    {
        if (onDataObjectReleased)
        {
            onDataObjectReleased();
        }
        return false;
    }
    auto *dataObject = new ShellDataObject(
        files,
        applicationMetadata,
        onceCallback(std::move(onDataObjectReleased)));
    dataObject->SetAsyncMode(TRUE);
    auto *dropSource = new ShellDropSource();
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT result = DoDragDrop(dataObject, dropSource, DROPEFFECT_COPY, &effect);
    dataObject->Release();
    dropSource->Release();
    OleUninitialize();
    return SUCCEEDED(result);
}

bool executeWindowsShellDrag(
    WindowsShellDragFileProvider fileProvider,
    const QByteArray &applicationMetadata,
    std::function<void()> onDataObjectReleased)
{
    if (!fileProvider || applicationMetadata.isEmpty())
    {
        if (onDataObjectReleased)
        {
            onDataObjectReleased();
        }
        return false;
    }
    const HRESULT initResult = OleInitialize(nullptr);
    if (FAILED(initResult))
    {
        if (onDataObjectReleased)
        {
            onDataObjectReleased();
        }
        return false;
    }
    auto *dataObject = new ShellDataObject(
        std::move(fileProvider),
        applicationMetadata,
        onceCallback(std::move(onDataObjectReleased)));
    dataObject->SetAsyncMode(TRUE);
    auto *dropSource = new ShellDropSource();
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT result = DoDragDrop(dataObject, dropSource, DROPEFFECT_COPY, &effect);
    dataObject->Release();
    dropSource->Release();
    OleUninitialize();
    return SUCCEEDED(result);
}

bool setWindowsShellClipboard(
    const std::vector<WindowsShellDragFile> &files,
    const QByteArray &applicationMetadata,
    std::function<void()> onDataObjectReleased)
{
    if (files.empty() || applicationMetadata.isEmpty())
    {
        if (onDataObjectReleased)
        {
            onDataObjectReleased();
        }
        return false;
    }
    const HRESULT initResult = OleInitialize(nullptr);
    if (FAILED(initResult))
    {
        if (onDataObjectReleased)
        {
            onDataObjectReleased();
        }
        return false;
    }
    auto *dataObject = new ShellDataObject(files, applicationMetadata, std::move(onDataObjectReleased));
    dataObject->SetAsyncMode(TRUE);
    const HRESULT result = OleSetClipboard(dataObject);
    if (SUCCEEDED(result))
    {
        std::lock_guard<std::mutex> lock(g_clipboardMutex);
        g_clipboardObject = dataObject;
    }
    dataObject->Release();
    OleUninitialize();
    return SUCCEEDED(result);
}

bool setWindowsShellClipboard(
    WindowsShellDragFileProvider fileProvider,
    const QByteArray &applicationMetadata,
    std::function<void()> onDataObjectReleased)
{
    if (!fileProvider || applicationMetadata.isEmpty())
    {
        if (onDataObjectReleased)
        {
            onDataObjectReleased();
        }
        return false;
    }
    const HRESULT initResult = OleInitialize(nullptr);
    if (FAILED(initResult))
    {
        if (onDataObjectReleased)
        {
            onDataObjectReleased();
        }
        return false;
    }
    auto *dataObject = new ShellDataObject(
        std::move(fileProvider),
        applicationMetadata,
        std::move(onDataObjectReleased));
    dataObject->SetAsyncMode(TRUE);
    const HRESULT result = OleSetClipboard(dataObject);
    if (SUCCEEDED(result))
    {
        std::lock_guard<std::mutex> lock(g_clipboardMutex);
        g_clipboardObject = dataObject;
    }
    dataObject->Release();
    OleUninitialize();
    return SUCCEEDED(result);
}

void clearWindowsShellClipboard()
{
    const HRESULT initResult = OleInitialize(nullptr);
    if (FAILED(initResult))
    {
        return;
    }

    IDataObject *clipboardObject = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_clipboardMutex);
        if (g_clipboardObject != nullptr)
        {
            clipboardObject = static_cast<IDataObject *>(g_clipboardObject);
            clipboardObject->AddRef();
        }
    }

    if (clipboardObject != nullptr)
    {
        if (OleIsCurrentClipboard(clipboardObject) == S_OK)
        {
            OleSetClipboard(nullptr);
        }
        clipboardObject->Release();
    }

    OleUninitialize();
}
