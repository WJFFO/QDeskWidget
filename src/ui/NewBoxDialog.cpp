/**
 * @file NewBoxDialog.cpp
 * 新建整理盒对话框的实现：纯代码布局（MColumnLayout / MRowLayout + UiStyle helpers）。
 *
 * 关键约定 / 坑：
 *  - 这里只收集输入、不碰磁盘；托管目录由 BoxManager::create() 在确认后才创建；
 *  - 「关联现有目录」模式下路径为空时直接 return 不 accept()，没有内联错误提示，
 *    用户需要自己注意到没选目录（刻意保持轻量，不做校验弹窗）；
 *  - 路径统一用 QDir::toNativeSeparators 回显，方便用户核对，入库前由 BoxManager 再转回 '/'。
 */
#include "ui/NewBoxDialog.h"

#include "core/AppSettings.h"
#include "core/Paths.h"
#include "mcolumnlayout.h"
#include "mrowlayout.h"
#include "ui/UiStyle.h"

#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>

// 构造即把整张表单搭完并接好信号。模态窗口，由 App::newBox 在栈上创建、exec() 后即销毁。
NewBoxDialog::NewBoxDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("新建整理盒"));
    setModal(true);
    UiStyle::styleDialog(this);
    // 最小宽 460：保证「路径框 + 浏览…」这一行放得下
    setMinimumWidth(460);

    const AppSettings &st = AppSettings::get();

    auto *root = new MColumnLayout(this);
    root->ColumnFillHost(this);
    root->SetMargin(20);
    root->SetSpacing(12);

    auto *heading = new QLabel(QStringLiteral("新建整理盒"), this);
    heading->setFont(st.uiFont(17, QFont::DemiBold));
    root->ColumnAddWidget(heading);

    // 这段说明是为了消除「会不会把我桌面文件搬走」的顾虑——实际只做目录引用
    auto *desc = new QLabel(
        QStringLiteral("整理盒背后是一个真实的文件夹，桌面上的文件不会被移动，"
                       "只是多了一个更顺手的入口。"),
        this);
    desc->setObjectName(QStringLiteral("hint"));
    desc->setFont(st.uiFont(12));
    desc->setWordWrap(true);
    root->ColumnAddWidget(desc);

    // 预填一个通用名并配合末尾的 selectAll()，用户可以直接覆写
    m_title = new QLineEdit(QStringLiteral("桌面整理"), this);
    m_title->setPlaceholderText(QStringLiteral("给这个整理盒起个名字"));
    root->ColumnAddWidget(UiStyle::formRow(this, QStringLiteral("名称"), m_title));

    // 默认走托管：这是推荐路径，用户不必先准备一个文件夹
    m_managed = new QRadioButton(QStringLiteral("新建托管文件夹（由本程序管理，推荐）"), this);
    m_managed->setChecked(true);
    root->ColumnAddWidget(m_managed);

    m_existing = new QRadioButton(QStringLiteral("关联电脑上已有的文件夹"), this);

    m_path = new QLineEdit(this);
    m_path->setPlaceholderText(QStringLiteral("选择一个文件夹…"));
    m_path->setEnabled(false); // 未选中「关联」前不可编辑，避免用户误以为能直接手打路径

    auto *browse = UiStyle::ghostButton(QStringLiteral("浏览…"), this);
    browse->setEnabled(false);

    auto *pathRow = new MRowLayout(this);
    pathRow->SetSpacing(8);
    pathRow->RowAddWidget(m_path, 1, Qt::AlignVCenter);
    // 浏览按钮钉在行尾；这里路径框 stretch=1，所以不会再触发「弹簧 1:1 抢空间」的偏移问题
    pathRow->RowPinToRight(browse, Qt::AlignVCenter);

    // 单选按钮 + 路径行放在同一个缩进的列里
    // 左边距 24 是为了让它们视觉上从属于上面的「新建 / 关联」二选一
    auto *pathCol = new MColumnLayout(this);
    pathCol->SetMargin(24, 0, 0, 0);
    pathCol->SetSpacing(8);
    pathCol->ColumnAddWidget(m_existing);
    pathCol->ColumnAddWidget(pathRow);
    root->ColumnAddWidget(pathCol);

    root->ColumnAddStretch(1);

    auto *cancel = UiStyle::ghostButton(QStringLiteral("取消"), this);
    auto *ok = UiStyle::primaryButton(QStringLiteral("创建"), this);
    root->ColumnAddWidget(UiStyle::buttonRow(this, { cancel, ok }));

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    // 确认前的唯一校验：关联模式下必须有路径。不满足就静默不动，对话框保持打开
    connect(ok, &QPushButton::clicked, this, [this]
    {
        if (m_existing->isChecked() && m_path->text().trimmed().isEmpty())
            return;
        accept();
    });
    // 模式切换：路径框与浏览按钮跟着启用/禁用；切到关联模式时把焦点送进路径框，
    // 省得用户还要再点一下
    connect(m_existing, &QRadioButton::toggled, this, [this, browse](bool on)
    {
        m_path->setEnabled(on);
        browse->setEnabled(on);
        if (on)
            m_path->setFocus();
        updateEnabled();
    });
    // 系统目录选择框；取消（返回空串）时保持原有输入不变
    connect(browse, &QPushButton::clicked, this, [this]
    {
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择文件夹"));
        if (!dir.isEmpty())
            m_path->setText(QDir::toNativeSeparators(dir));
    });

    // 打开即聚焦名称并全选，方便直接覆盖默认的「桌面整理」
    m_title->setFocus();
    m_title->selectAll();
}

// 目前是空实现：启用/禁用逻辑直接写在 m_existing 的 toggled lambda 里（见构造）。
// 保留调用点，将来增加更多互斥项时可以集中放在这里。
void NewBoxDialog::updateEnabled() {}

// 空名称回落成「新整理盒」，与 BoxManager 侧的默认标题保持一致
QString NewBoxDialog::boxTitle() const
{
    const QString t = m_title->text().trimmed();
    return t.isEmpty() ? QStringLiteral("新整理盒") : t;
}

// 只回显用户输入，不做存在性校验——目录是否有效由 BoxManager / 窗口创建时再判断
QString NewBoxDialog::folder() const
{
    return m_path->text().trimmed();
}

// 用「是否选了关联」取反：两个单选按钮互斥，托管是默认项
bool NewBoxDialog::isManaged() const
{
    return !m_existing->isChecked();
}
