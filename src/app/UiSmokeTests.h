#ifndef DIRBRIDGE_APP_UISMOKETESTS_H
#define DIRBRIDGE_APP_UISMOKETESTS_H

class MainWindow;

/**
 * @brief Verifies remote UI object wiring and default state.
 * @param window Main window under test.
 * @return true when the UI objects are present and correctly initialized.
 */
bool checkRemoteUiObjects(MainWindow &window);

/**
 * @brief Runs the fake-backend remote UI workflow smoke test.
 * @param window Main window under test.
 * @return true when the fake workflow passes.
 */
bool checkRemoteUiWorkflow(MainWindow &window);

/**
 * @brief Runs the live remote connection and navigation smoke test.
 * @param window Main window under test.
 * @return true when the live workflow passes.
 */
bool checkLiveRemoteUiWorkflow(MainWindow &window);

/**
 * @brief Runs the live remote refresh smoke test.
 * @param window Main window under test.
 * @return true when refresh detects external server-side changes.
 */
bool checkLiveRemoteRefreshWorkflow(MainWindow &window);

/**
 * @brief Runs the live upload/download transfer UI smoke test.
 * @param window Main window under test.
 * @return true when transfer UI workflow passes.
 */
bool checkLiveRemoteTransferWorkflow(MainWindow &window);

#endif // DIRBRIDGE_APP_UISMOKETESTS_H
