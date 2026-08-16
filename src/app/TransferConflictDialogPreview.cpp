#include "ui/TransferConflictDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QLineEdit>
#include <QStringList>
#include <QTextStream>

namespace
{
TransferConflictDialog::Options previewOptions(bool download, bool directory)
{
    TransferConflictDialog::Options options;
    options.direction = download
        ? TransferConflictDialog::Direction::Download
        : TransferConflictDialog::Direction::Upload;
    options.source.name = directory ? "新建文件夹 (3)" : "示例文件.txt";
    options.source.path = download ? "/home/testuser/示例文件.txt" : "E:/传输测试/示例文件.txt";
    options.source.kind = directory
        ? TransferConflictDialog::ItemKind::Directory
        : TransferConflictDialog::ItemKind::File;
    options.source.size = directory ? -1 : 1536;
    options.source.modifiedTime = QDateTime::fromString("2026-08-14 22:08:00", "yyyy-MM-dd HH:mm:ss");
    options.target = options.source;
    options.target.path = download ? "E:/下载/示例文件.txt" : "/home/testuser/示例文件.txt";
    options.target.size = directory ? -1 : 2048;
    options.target.modifiedTime = QDateTime::fromString("2026-08-14 22:02:57", "yyyy-MM-dd HH:mm:ss");
    options.allowOverwrite = !directory;
    options.allowApplyToAll = true;
    options.suggestedName = directory ? "新建文件夹 (4)" : "示例文件 (1).txt";
    return options;
}

bool checkDialog(
    const TransferConflictDialog::Options &options,
    const QString &expectedTitle,
    const QStringList &expectedActions,
    TransferConflictDialog::Action expectedDefaultAction)
{
    TransferConflictDialog dialog(options);
    auto *actionCombo = dialog.findChild<QComboBox *>("conflictActionCombo");
    auto *applyToAll = dialog.findChild<QCheckBox *>("conflictApplyToAllCheckBox");
    auto *renameEdit = dialog.findChild<QLineEdit *>("conflictRenameEdit");
    if (dialog.windowTitle() != expectedTitle || actionCombo == nullptr || applyToAll == nullptr || renameEdit == nullptr)
    {
        QTextStream(stderr) << "传输冲突对话框基础控件或标题不符合预期" << Qt::endl;
        return false;
    }

    QStringList actualActions;
    for (int index = 0; index < actionCombo->count(); ++index)
    {
        actualActions.append(actionCombo->itemText(index));
    }
    if (actualActions != expectedActions
        || actionCombo->currentData().toInt() != static_cast<int>(expectedDefaultAction))
    {
        QTextStream(stderr) << "传输冲突动作顺序或默认动作不符合预期" << Qt::endl;
        return false;
    }

    for (const QString &action : actualActions)
    {
        if (action.contains("继续"))
        {
            QTextStream(stderr) << "传输冲突动作中不应出现继续传输选项" << Qt::endl;
            return false;
        }
    }

    if (!renameEdit->isHidden())
    {
        QTextStream(stderr) << "非重命名动作不应显示名称输入框" << Qt::endl;
        return false;
    }

    const int renameIndex = actionCombo->findData(static_cast<int>(TransferConflictDialog::Action::Rename));
    actionCombo->setCurrentIndex(renameIndex);
    QApplication::processEvents();
    if (renameIndex < 0 || renameEdit->isHidden())
    {
        QTextStream(stderr) << "选择重命名后应显示名称输入框" << Qt::endl;
        return false;
    }
    return true;
}

bool runChecks()
{
    const bool uploadFileOk = checkDialog(
        previewOptions(false, false),
        "要上传的文件存在",
        {"覆盖", "跳过", "重命名"},
        TransferConflictDialog::Action::Overwrite);
    const bool downloadFileOk = checkDialog(
        previewOptions(true, false),
        "要下载的文件存在",
        {"覆盖", "跳过", "重命名"},
        TransferConflictDialog::Action::Overwrite);
    const bool directoryOk = checkDialog(
        previewOptions(false, true),
        "要上传的文件存在",
        {"跳过", "重命名"},
        TransferConflictDialog::Action::Skip);
    return uploadFileOk && downloadFileOk && directoryOk;
}
}

int main(int argc, char *argv[])
{
    QApplication application(argc, argv);
    application.setApplicationName("DirBridge 传输冲突对话框预览");

    const QStringList arguments = application.arguments();
    const bool download = arguments.contains("--download");
    const bool directory = arguments.contains("--directory");

    if (arguments.contains("--check"))
    {
        return runChecks() ? 0 : 1;
    }

    const TransferConflictDialog::Options options = previewOptions(download, directory);

    TransferConflictDialog dialog(options);
    const int renderIndex = arguments.indexOf("--render");
    if (renderIndex >= 0 && renderIndex + 1 < arguments.size())
    {
        dialog.show();
        QApplication::processEvents();
        const QString outputPath = QDir::fromNativeSeparators(arguments.at(renderIndex + 1));
        return dialog.grab().save(outputPath) ? 0 : 1;
    }

    return dialog.exec() == QDialog::Accepted ? 0 : 0;
}
