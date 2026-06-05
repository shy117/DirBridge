#ifndef DIRBRIDGE_UI_MAINWINDOW_H
#define DIRBRIDGE_UI_MAINWINDOW_H

#include <QMainWindow>

#include <memory>
#include <vector>

#include "config/SiteProfile.h"
#include "config/SiteStore.h"
#include "core/DependencyCheck.h"
#include "core/FakeRemoteFileSystem.h"

class QComboBox;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class FilePanel;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(const DependencyCheckResult &dependencyCheck, QWidget *parent = nullptr);

private:
    void loadSites();
    void saveSites();
    void setupMenuBar();
    void setupToolBar();
    void setupQuickConnectBar();
    void setupCentralWorkspace(const DependencyCheckResult &dependencyCheck);
    QTreeWidget *createSessionManager();
    void populateSessionManager();
    void appendLog(const QString &level, const QString &message);
    SiteProfile profileFromQuickConnect() const;
    void connectQuickProfile(bool saveProfile);
    void showRemoteProfile(const SiteProfile &profile);
    void fillQuickConnectFromItem(QTreeWidgetItem *item);
    QString siteDisplayName(const SiteProfile &profile) const;

private:
    SiteStore m_siteStore;
    std::vector<SiteProfile> m_sites;
    std::unique_ptr<FakeRemoteFileSystem> m_remoteFileSystem;

    QComboBox *m_protocolCombo = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QLineEdit *m_portEdit = nullptr;
    QLineEdit *m_userEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_saveSiteButton = nullptr;
    QTreeWidget *m_sessionTree = nullptr;
    QTreeWidget *m_logView = nullptr;
    FilePanel *m_remotePanel = nullptr;
};

#endif // DIRBRIDGE_UI_MAINWINDOW_H
