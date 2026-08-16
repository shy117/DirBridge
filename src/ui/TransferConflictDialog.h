#ifndef DIRBRIDGE_UI_TRANSFER_CONFLICT_DIALOG_H
#define DIRBRIDGE_UI_TRANSFER_CONFLICT_DIALOG_H

#include <QDateTime>
#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;

class TransferConflictDialog final : public QDialog
{
public:
    enum class Direction
    {
        Upload,
        Download
    };

    enum class ItemKind
    {
        File,
        Directory
    };

    enum class Action
    {
        Cancel,
        Overwrite,
        Skip,
        Rename
    };

    struct ItemDetails
    {
        QString name;
        QString path;
        ItemKind kind = ItemKind::File;
        qint64 size = -1;
        QDateTime modifiedTime;
    };

    struct Options
    {
        Direction direction = Direction::Upload;
        ItemDetails source;
        ItemDetails target;
        bool allowOverwrite = true;
        bool allowApplyToAll = true;
        QString suggestedName;
    };

    struct Decision
    {
        Action action = Action::Cancel;
        QString newName;
        bool applyToAll = false;
    };

    explicit TransferConflictDialog(const Options &options, QWidget *parent = nullptr);

    Decision decision() const;

private:
    static QString directionTitle(Direction direction);
    static QString itemDetailsText(const ItemDetails &details);
    void updateRenameEditor();
    void updateConfirmButton();

    Options m_options;
    QComboBox *m_actionCombo = nullptr;
    QCheckBox *m_applyToAllCheckBox = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QPushButton *m_confirmButton = nullptr;
    QString m_renameName;
};

#endif // DIRBRIDGE_UI_TRANSFER_CONFLICT_DIALOG_H
