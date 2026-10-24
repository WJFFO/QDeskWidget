/**
 * @file NewBoxDialog.h
 * 「新建整理盒」对话框的声明：只在名字 / 目录两项上做选择，不做任何磁盘操作。
 *
 * 关键约定 / 坑：
 *  - 对话框只负责收集输入，真正的建目录 / 建档在 BoxManager::create() 里；
 *  - 两种模式互斥：托管（程序在 storage/<id> 下建目录）与关联现有目录；
 *    关联模式下路径为空时不会 accept()，但也不会提示——由调用方（App）拿不到值自然放弃；
 *  - 模态对话框，exec() 返回 QDialog::Accepted 才应继续。
 */
#pragma once

#include <QDialog>

class QLineEdit;
class QRadioButton;

/**
 * 新建整理盒对话框：
 *  - 新建托管文件夹（在数据目录下自动创建）
 *  - 或关联现有文件夹
 * 布局使用 MColumnLayout / MRowLayout。
 *
 * 两类输入共用一个「名称」框：托管模式下名称只当标题，关联模式下同样不影响目录，
 * 因此 boxTitle() 与 folder() 相互独立，不互相推导。
 */
class NewBoxDialog : public QDialog
{
    Q_OBJECT

public:
    /** @param parent 父窗口；正常传 nullptr，由 App::newBox 在栈上创建并 exec() */
    explicit NewBoxDialog(QWidget *parent = nullptr);

    /** @return 用户输入的名称（已去首尾空白）；留空时回落成「新整理盒」，不会返回空串 */
    QString boxTitle() const;
    /**
     * @return 关联模式的目录文本（已去首尾空白，且已转成 Windows 原生分隔符）；
     *         托管模式下该框不可编辑，通常返回空串，调用方应忽略它
     */
    QString folder() const;
    /**
     * @return true 表示「新建托管文件夹」（默认选项），false 表示「关联已有文件夹」。
     *         注意它以「是否选中关联」取反得出，而关联单选按钮嵌在缩进列里，默认未选中。
     */
    bool isManaged() const;

private:
    /**
     * 预留的联动刷新入口：根据当前模式调整控件启用状态。
     * 目前是空实现——路径框与浏览按钮的启用/禁用直接写在 m_existing 的 toggled 连接里。
     * 保留这个调用点是为了将来加更多互斥项时不必改信号连接。
     */
    void updateEnabled();

    QLineEdit *m_title = nullptr;        // 名称输入框，构造时预填「桌面整理」并全选，方便直接覆盖输入
    QRadioButton *m_managed = nullptr;   // 「新建托管文件夹」，默认选中；选中时路径框与浏览按钮禁用
    QRadioButton *m_existing = nullptr;  // 「关联已有文件夹」；只有选中它才会启用路径输入
    QLineEdit *m_path = nullptr;         // 现有目录路径，初始禁用；由「浏览…」写入 native 分隔符形式
};
