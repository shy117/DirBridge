#include "ui/FileChangeMonitor.h"

#include <QFileInfo>

namespace
{
constexpr int stabilityDelayMilliseconds = 400;
}

FileChangeMonitor::FileChangeMonitor(QObject *parent)
    : QObject(parent)
{
    m_debounceTimer.setSingleShot(true);
    m_debounceTimer.setInterval(stabilityDelayMilliseconds);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &FileChangeMonitor::onFileChanged);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &FileChangeMonitor::onDirectoryChanged);
    connect(&m_debounceTimer, &QTimer::timeout, this, &FileChangeMonitor::onDebounceTimeout);
}

void FileChangeMonitor::startMonitoring(const QString &filePath)
{
    stopMonitoring();

    const QFileInfo info(filePath);
    m_filePath = info.absoluteFilePath();
    m_directoryPath = info.absolutePath();
    m_lastObservedSignature = currentSignature();
    m_lastEmittedGeneration = m_changeGeneration;
    ensureWatchPaths();
}

void FileChangeMonitor::stopMonitoring()
{
    m_debounceTimer.stop();
    const QStringList files = m_watcher.files();
    if (!files.isEmpty())
    {
        m_watcher.removePaths(files);
    }
    const QStringList directories = m_watcher.directories();
    if (!directories.isEmpty())
    {
        m_watcher.removePaths(directories);
    }
    m_filePath.clear();
    m_directoryPath.clear();
    m_lastObservedSignature = {};
}

bool FileChangeMonitor::isMonitoring() const
{
    return !m_filePath.isEmpty();
}

QString FileChangeMonitor::filePath() const
{
    return m_filePath;
}

void FileChangeMonitor::onFileChanged(const QString &path)
{
    Q_UNUSED(path);
    ++m_changeGeneration;
    scheduleStabilityCheck();
}

void FileChangeMonitor::onDirectoryChanged(const QString &path)
{
    Q_UNUSED(path);

    if (!isMonitoring())
    {
        return;
    }

    const bool wasWorkingFileWatched = m_watcher.files().contains(m_filePath);
    const FileSignature signature = currentSignature();
    ensureWatchPaths();
    if (wasWorkingFileWatched && signature == m_lastObservedSignature)
    {
        return;
    }

    ++m_changeGeneration;
    scheduleStabilityCheck();
}

void FileChangeMonitor::onDebounceTimeout()
{
    if (!isMonitoring())
    {
        return;
    }

    ensureWatchPaths();
    const FileSignature signature = currentSignature();
    if (!signature.exists)
    {
        Q_EMIT fileUnavailable(m_filePath);
        return;
    }

    if (signature != m_lastObservedSignature)
    {
        m_lastObservedSignature = signature;
        scheduleStabilityCheck();
        return;
    }

    if (m_changeGeneration == m_lastEmittedGeneration)
    {
        return;
    }

    m_lastEmittedGeneration = m_changeGeneration;
    Q_EMIT stableFileChanged(m_filePath);
}

bool FileChangeMonitor::FileSignature::operator==(const FileSignature &other) const
{
    return exists == other.exists
        && size == other.size
        && modifiedAtMilliseconds == other.modifiedAtMilliseconds;
}

bool FileChangeMonitor::FileSignature::operator!=(const FileSignature &other) const
{
    return !(*this == other);
}

FileChangeMonitor::FileSignature FileChangeMonitor::currentSignature() const
{
    const QFileInfo info(m_filePath);
    if (!info.exists() || !info.isFile())
    {
        return {};
    }
    return {true, info.size(), info.lastModified().toMSecsSinceEpoch()};
}

void FileChangeMonitor::ensureWatchPaths()
{
    if (m_directoryPath.isEmpty())
    {
        return;
    }

    if (!m_watcher.directories().contains(m_directoryPath))
    {
        m_watcher.addPath(m_directoryPath);
    }
    if (currentSignature().exists && !m_watcher.files().contains(m_filePath))
    {
        m_watcher.addPath(m_filePath);
    }
}

void FileChangeMonitor::scheduleStabilityCheck()
{
    if (isMonitoring())
    {
        m_debounceTimer.start();
    }
}
