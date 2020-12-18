#ifndef DISASMWINDOW_H
#define DISASMWINDOW_H

#include <QDockWidget>
#include <QTableView>
#include <QMenu>
#include "disassembler.h"
#include "breakpoint.h"
#include "memory.h"

class TargetModel;
class Dispatcher;

class DisasmTableModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column
    {
        kColSymbol,
        kColAddress,
        kColBreakpoint,
        kColDisasm,
        kColComments,

        kColCount
    };

    DisasmTableModel(QObject * parent, TargetModel* pTargetModel, Dispatcher* m_pDispatcher);

    // "When subclassing QAbstractTableModel, you must implement rowCount(), columnCount(), and data()."
    virtual int rowCount(const QModelIndex &parent) const;
    virtual int columnCount(const QModelIndex &parent) const;
    virtual QVariant data(const QModelIndex &index, int role) const;
    virtual QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const;

    // "The model emits signals to indicate changes. For example, dataChanged() is emitted whenever items of data made available by the model are changed"
    // So I expect we can emit that if we see the target has changed

    uint32_t GetAddress() const { return m_logicalAddr; }
    int GetRowCount() const     { return m_rowCount; }

    bool SetAddress(std::string addr);
    void MoveUp();
    void MoveDown();
    void PageUp();
    void PageDown();
    void RunToRow(int row);
    void ToggleBreakpoint(int row);
    void SetRowCount(int count);

signals:
    void addressChanged(uint64_t addr);

private slots:
    void startStopChangedSlot();
    void memoryChangedSlot(int memorySlot, uint64_t commandId);
    void breakpointsChangedSlot(uint64_t commandId);
    void symbolTableChangedSlot(uint64_t commandId);

private:
    void SetAddress(uint32_t addr);
    void RequestMemory();
    void CalcDisasm();
    void printEA(const operand &op, const Registers &regs, uint32_t address, QTextStream &ref) const;
    TargetModel* m_pTargetModel;
    Dispatcher*  m_pDispatcher;

    // Cached data when the up-to-date request comes through
    Memory       m_memory;
    Disassembler::disassembly m_disasm;
    Breakpoints m_breakpoints;
    int         m_rowCount;

    // Address of the top line of text that was requested

    uint32_t m_requestedAddress;    // Most recent address requested
    uint32_t m_logicalAddr;    // Most recent address that can be shown
    uint64_t m_requestId;           // Most recent memory request

    QPixmap m_breakpoint10Pixmap;
    static const uint32_t kInvalid = 0xffffffff;
};

class DisasmTableView : public QTableView
{
    Q_OBJECT
public:
    DisasmTableView(QWidget* parent, DisasmTableModel* pModel, TargetModel* pTargetModel);

public slots:
    void runToCursor();
    void toggleBreakpoint();

protected:
    QModelIndex moveCursor(QAbstractItemView::CursorAction cursorAction, Qt::KeyboardModifiers modifiers);
private:
    virtual void contextMenuEvent(QContextMenuEvent *event);

    void runToCursorRightClick();
    void toggleBreakpointRightClick();

    // override -- this doesn't trigger at the start?
    virtual void resizeEvent(QResizeEvent*);
private slots:
    void RecalcRowCount();

private:
    DisasmTableModel*     m_pTableModel;
    // Actions
    QAction*              m_pRunUntilAction;
    QAction*              m_pBreakpointAction;
    QMenu                 m_rightClickMenu;

    // Remembers which row we right-clicked on
    int                   m_rightClickRow;
};

class DisasmWidget : public QDockWidget
{
    Q_OBJECT
public:
    DisasmWidget(QWidget *parent, TargetModel* pTargetModel, Dispatcher* m_pDispatcher);

protected:

protected slots:
    void cellClickedSlot(const QModelIndex& index);

    void keyDownPressed();
    void keyUpPressed();
    void keyPageDownPressed();
    void keyPageUpPressed();
    void returnPressedSlot();
    void textChangedSlot();

private:

    void UpdateTextBox();

    QLineEdit*      m_pLineEdit;
    QTableView*     m_pTableView;

    DisasmTableModel* m_pTableModel;
    TargetModel*    m_pTargetModel;
    Dispatcher*     m_pDispatcher;
};

#endif // DISASMWINDOW_H