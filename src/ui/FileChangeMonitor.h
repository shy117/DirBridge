#ifndef DIRBRIDGE_UI_FILECHANGEMONITOR_H
#define DIRBRIDGE_UI_FILECHANGEMONITOR_H

#include <QDateTime>
#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>

/**
 * @brief 监控外部编辑器写入的缓存文件，并在内容稳定后通知调用方。
 *
 * 同时监控文件和父目录，以适配编辑器使用临时文件替换原文件的保存方式。
 */
class FileChangeMonitor : public QObject
{
    Q_OBJECT

public:
    explicit FileChangeMonitor(QObject *parent = nullptr);

    void startMonitoring(const QString &filePath);
    void stopMonitoring();
    bool isMonitoring() const;
    QString filePath() const;

Q_SIGNALS:
    void stableFileChanged(const QString &filePath);
    void fileUnavailable(const QString &filePath);

private:
    void onFileChanged(const QString &path);
    void onDirectoryChanged(const QString &path);
    void onDebounceTimeout();

private:
    struct FileSignature
    {
        bool exists = false;
        qint64 size = -1;
        qint64 modifiedAtMilliseconds = -1;

        bool operator==(const FileSignature &other) const;
        bool operator!=(const FileSignature &other) const;
    };

    FileSignature currentSignature() const;
    void ensureWatchPaths();
    void scheduleStabilityCheck();

    QFileSystemWatcher m_watcher;
    QTimer m_debounceTimer;
    QString m_filePath;
    QString m_directoryPath;
    FileSignature m_lastObservedSignature;
    quint64 m_changeGeneration = 0;
    quint64 m_lastEmittedGeneration = 0;
};

#endif // DIRBRIDGE_UI_FILECHANGEMONITOR_H
