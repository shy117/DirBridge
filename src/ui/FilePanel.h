#ifndef DIRBRIDGE_UI_FILEPANEL_H
#define DIRBRIDGE_UI_FILEPANEL_H

#include <QFileIconProvider>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;

class FilePanel : public QWidget
{
    Q_OBJECT

public:
    enum class Mode
    {
        Local,
        RemotePlaceholder
    };

    explicit FilePanel(Mode mode, QWidget *parent = nullptr);

    void setRemoteSummary(const QString &curlVersion, bool hasFtp, bool hasSftp);

private:
    void setupUi();
    void connectSignals();
    void initialize();
    void navigateTo(const QString &path, bool addToHistory = true);
    void refresh();
    void navigateUp();
    void navigateBack();
    void navigateForward();
    void updateNavigationButtons();
    void populateLocalDirectory(const QString &path);
    void populateRemotePlaceholder();
    QTableWidgetItem *createItem(const QString &text, const QIcon &icon = QIcon()) const;
    QString selectedEntryPath() const;

private:
    Mode m_mode;
    QString m_currentPath;
    QStringList m_history;
    int m_historyIndex = -1;
    QFileIconProvider m_iconProvider;

    QPushButton *m_backButton = nullptr;
    QPushButton *m_forwardButton = nullptr;
    QPushButton *m_upButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QLabel *m_stateLabel = nullptr;
    QTableWidget *m_table = nullptr;
};

#endif // DIRBRIDGE_UI_FILEPANEL_H
