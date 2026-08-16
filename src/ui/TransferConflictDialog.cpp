#include "ui/TransferConflictDialog.h"

#include "ui/panel_shared.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace
{
QFrame *createSeparator(QWidget *parent)
{
    auto *separator = new QFrame(parent);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    return separator;
}

QLabel *createItemIcon(const TransferConflictDialog::ItemDetails &details, QWidget *parent)
{
    auto *iconLabel = new QLabel(parent);
    const QStyle::StandardPixmap standardPixmap = details.kind == TransferConflictDialog::ItemKind::Directory
        ? QStyle::SP_DirIcon
        : QStyle::SP_FileIcon;
    iconLabel->setPixmap(parent->style()->standardIcon(standardPixmap).pixmap(32, 32));
    iconLabel->setFixedSize(42, 38);
    iconLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    return iconLabel;
}
}

TransferConflictDialog::TransferConflictDialog(const Options &options, QWidget *parent)
    : QDialog(parent),
      m_options(options)
{
    setObjectName("transferConflictDialog");
    setWindowTitle(directionTitle(options.direction));
    setMinimumWidth(440);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 14);
    layout->setSpacing(12);
    layout->setSizeConstraint(QLayout::SetFixedSize);

    auto *summaryLayout = new QHBoxLayout();
    summaryLayout->setSpacing(16);
    auto *warningIcon = new QLabel(this);
    warningIcon->setObjectName("conflictWarningIcon");
    warningIcon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(32, 32));
    warningIcon->setFixedSize(48, 40);
    warningIcon->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    summaryLayout->addWidget(warningIcon, 0, Qt::AlignTop);

    auto *summaryText = new QLabel("此文件夹已包含以下名称的对象。\n请选择要执行的操作。", this);
    summaryText->setObjectName("conflictInstructionLabel");
    summaryText->setWordWrap(true);
    summaryLayout->addWidget(summaryText, 1);
    layout->addLayout(summaryLayout);

    auto *nameLayout = new QHBoxLayout();
    auto *nameCaption = new QLabel("名称(N)：", this);
    nameCaption->setMinimumWidth(78);
    nameLayout->addWidget(nameCaption);
    m_nameEdit = new QLineEdit(options.source.name, this);
    m_nameEdit->setObjectName("conflictNameEdit");
    QFont nameFont = m_nameEdit->font();
    nameFont.setBold(true);
    m_nameEdit->setFont(nameFont);
    m_nameEdit->setFixedHeight(m_nameEdit->sizeHint().height());
    m_nameEdit->setReadOnly(true);
    m_nameEdit->setFrame(false);
    nameLayout->addWidget(m_nameEdit, 1);
    layout->addLayout(nameLayout);

    layout->addWidget(createSeparator(this));

    const auto addItemSection = [this, layout](
                                    const QString &caption,
                                    const QString &objectPrefix,
                                    const ItemDetails &details) {
        auto *captionLabel = new QLabel(caption, this);
        captionLabel->setObjectName(objectPrefix + "Caption");
        layout->addWidget(captionLabel);

        auto *itemLayout = new QHBoxLayout();
        itemLayout->setSpacing(16);
        auto *icon = createItemIcon(details, this);
        icon->setObjectName(objectPrefix + "Icon");
        itemLayout->addWidget(icon, 0, Qt::AlignTop);

        auto *detailsLabel = new QLabel(itemDetailsText(details), this);
        detailsLabel->setObjectName(objectPrefix + "Details");
        detailsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        detailsLabel->setWordWrap(true);
        detailsLabel->setToolTip(details.path);
        itemLayout->addWidget(detailsLabel, 1, Qt::AlignVCenter);
        layout->addLayout(itemLayout);
    };

    addItemSection("目标：", "conflictTarget", options.target);
    addItemSection("源：", "conflictSource", options.source);

    layout->addWidget(createSeparator(this));

    auto *actionLayout = new QHBoxLayout();
    auto *actionCaption = new QLabel("操作(C)：", this);
    actionCaption->setMinimumWidth(78);
    actionLayout->addWidget(actionCaption);

    m_actionCombo = new QComboBox(this);
    m_actionCombo->setObjectName("conflictActionCombo");
    if (options.allowOverwrite)
    {
        m_actionCombo->addItem("覆盖", static_cast<int>(Action::Overwrite));
    }
    m_actionCombo->addItem("跳过", static_cast<int>(Action::Skip));
    m_actionCombo->addItem("重命名", static_cast<int>(Action::Rename));
    const Action defaultAction = options.allowOverwrite ? Action::Overwrite : Action::Skip;
    m_actionCombo->setCurrentIndex(m_actionCombo->findData(static_cast<int>(defaultAction)));
    m_actionCombo->setMinimumWidth(144);
    actionLayout->addWidget(m_actionCombo);

    m_applyToAllCheckBox = new QCheckBox("全部应用(A)", this);
    m_applyToAllCheckBox->setObjectName("conflictApplyToAllCheckBox");
    m_applyToAllCheckBox->setVisible(options.allowApplyToAll);
    actionLayout->addWidget(m_applyToAllCheckBox);
    actionLayout->addStretch(1);
    layout->addLayout(actionLayout);

    m_renameName = options.suggestedName.isEmpty() ? options.source.name : options.suggestedName;

    auto *buttons = new QDialogButtonBox(this);
    m_confirmButton = buttons->addButton("确定", QDialogButtonBox::AcceptRole);
    m_confirmButton->setObjectName("conflictConfirmButton");
    m_confirmButton->setDefault(true);
    auto *cancelButton = buttons->addButton("取消", QDialogButtonBox::RejectRole);
    cancelButton->setObjectName("conflictCancelButton");
    layout->addWidget(buttons);

    connect(m_actionCombo, &QComboBox::currentIndexChanged, this, [this]() {
        updateRenameEditor();
    });
    connect(m_nameEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (static_cast<Action>(m_actionCombo->currentData().toInt()) == Action::Rename)
        {
            m_renameName = text;
        }
        updateConfirmButton();
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    updateRenameEditor();
}

TransferConflictDialog::Decision TransferConflictDialog::decision() const
{
    Decision result;
    if (QDialog::result() != QDialog::Accepted || m_actionCombo == nullptr)
    {
        return result;
    }

    result.action = static_cast<Action>(m_actionCombo->currentData().toInt());
    result.applyToAll = m_options.allowApplyToAll && m_applyToAllCheckBox->isChecked();
    if (result.action == Action::Rename)
    {
        result.newName = m_nameEdit->text().trimmed();
    }
    return result;
}

QString TransferConflictDialog::directionTitle(Direction direction)
{
    return direction == Direction::Upload ? "要上传的文件存在" : "要下载的文件存在";
}

QString TransferConflictDialog::itemDetailsText(const ItemDetails &details)
{
    QStringList lines;
    lines.append(details.modifiedTime.isValid()
            ? details.modifiedTime.toString("yyyy年M月d日, HH:mm:ss")
            : QString("修改时间未知"));
    if (details.kind == ItemKind::File && details.size >= 0)
    {
        lines.append(panel_shared::formatFileSize(details.size));
    }
    return lines.join('\n');
}

void TransferConflictDialog::updateRenameEditor()
{
    const Action action = static_cast<Action>(m_actionCombo->currentData().toInt());
    const bool renaming = action == Action::Rename;
    m_nameEdit->setReadOnly(!renaming);
    m_nameEdit->setFrame(renaming);
    if (renaming)
    {
        m_nameEdit->setFocusPolicy(Qt::StrongFocus);
        m_nameEdit->setStyleSheet({});
        m_nameEdit->setText(m_renameName);
        m_nameEdit->setFocus();
        m_nameEdit->selectAll();
    }
    else
    {
        m_nameEdit->setFocusPolicy(Qt::NoFocus);
        m_nameEdit->setStyleSheet(
            "QLineEdit#conflictNameEdit { background: transparent; border: none; padding: 0; }");
        m_nameEdit->setText(m_options.source.name);
    }
    updateConfirmButton();
}

void TransferConflictDialog::updateConfirmButton()
{
    const Action action = static_cast<Action>(m_actionCombo->currentData().toInt());
    m_confirmButton->setEnabled(action != Action::Rename || !m_nameEdit->text().trimmed().isEmpty());
}
