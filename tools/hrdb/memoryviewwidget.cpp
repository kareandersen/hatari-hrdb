#include "memoryviewwidget.h"

#include <iostream>
#include <QGroupBox>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QVBoxLayout>
#include <QHeaderView>
#include <QStringListModel>
#include <QFontDatabase>
#include <QCompleter>
#include <QPainter>
#include <QKeyEvent>

#include "dispatcher.h"
#include "targetmodel.h"
#include "stringparsers.h"
#include "symboltablemodel.h"

MemoryWidget::MemoryWidget(QWidget *parent, TargetModel *pTargetModel, Dispatcher* pDispatcher,
                                           int windowIndex) :
    QWidget(parent),
    m_pTargetModel(pTargetModel),
    m_pDispatcher(pDispatcher),
    m_isLocked(false),
    m_address(0),
    m_bytesPerRow(16),
    m_mode(kModeByte),
    m_rowCount(1),
    m_requestId(0),
    m_windowIndex(windowIndex),
    m_cursorRow(0),
    m_cursorCol(0)
{
    m_memSlot = (MemorySlot)(MemorySlot::kMemoryView0 + m_windowIndex);

    RecalcSizes();
    SetMode(Mode::kModeByte);
    setFocus();
    setFocusPolicy(Qt::StrongFocus);
    connect(m_pTargetModel, &TargetModel::memoryChangedSignal,      this, &MemoryWidget::memoryChangedSlot);
    connect(m_pTargetModel, &TargetModel::startStopChangedSignal,   this, &MemoryWidget::startStopChangedSlot);
    connect(m_pTargetModel, &TargetModel::connectChangedSignal,     this, &MemoryWidget::connectChangedSlot);
}

bool MemoryWidget::SetAddress(std::string expression)
{
    uint32_t addr;
    if (!StringParsers::ParseExpression(expression.c_str(), addr,
                                        m_pTargetModel->GetSymbolTable(),
                                        m_pTargetModel->GetRegs()))
    {
        return false;
    }
    SetAddress(addr);
    m_addressExpression = expression;
    return true;
}

void MemoryWidget::SetAddress(uint32_t address)
{
    m_address = address;
    RequestMemory();
}

void MemoryWidget::SetRowCount(uint32_t rowCount)
{
    if (rowCount != m_rowCount)
    {
        m_rowCount = rowCount;
        RequestMemory();

        if (m_cursorRow >= m_rowCount)
        {
            m_cursorRow = m_rowCount - 1;
        }
        repaint();
    }
}

void MemoryWidget::SetLock(bool locked)
{
    if (!locked != m_isLocked)
    {
        if (locked)
        {
            // Recalculate this expression for locking
            SetAddress(m_addressExpression);
        }
    }
    m_isLocked = locked;
}

void MemoryWidget::SetMode(MemoryWidget::Mode mode)
{
    m_mode = mode;

    // Calc the screen postions.
    // I need a screen position for each *character* on the grid (i.e. nybble)

    uint32_t groupSize = 0;
    if (m_mode == kModeByte)
        groupSize = 1;
    else if (m_mode == kModeWord)
        groupSize = 2;
    else if (m_mode == kModeLong)
        groupSize = 4;

    m_xpos.clear();
    for (uint32_t i = 0; i < m_bytesPerRow; ++i)
    {
        uint32_t group = i / groupSize;     // group of N bytes (N * chars)
        uint32_t byteInGroup = i % groupSize;

        // Calc the left-hand X position of this byte
        uint32_t base_x = group * (groupSize * 2 + 1) + 2 * byteInGroup;

        // top nybble
        m_xpos.push_back(base_x);
        // bottom nybble
        m_xpos.push_back(base_x + 1);
    }

    RecalcText();
}

void MemoryWidget::MoveUp()
{
    if (m_cursorRow > 0)
    {
        --m_cursorRow;
        repaint();
        return;
    }

    if (m_requestId != 0)
        return; // not up to date

    if (m_address >= m_bytesPerRow)
        SetAddress(m_address - m_bytesPerRow);
    else
        SetAddress(0);
}

void MemoryWidget::MoveDown()
{
    if (m_cursorRow < m_rowCount - 1)
    {
        ++m_cursorRow;
        repaint();
        return;
    }

    if (m_requestId != 0)
        return; // not up to date

    SetAddress(m_address + m_bytesPerRow);
}

void MemoryWidget::MoveLeft()
{
    if (m_cursorCol == 0)
        return;

    m_cursorCol--;
    repaint();
}

void MemoryWidget::MoveRight()
{
    if (m_cursorCol + 2 >= m_bytesPerRow * 2 + 1)
        return;

    m_cursorCol++;
    repaint();
}

void MemoryWidget::PageUp()
{
    if (m_cursorRow > 0)
    {
        m_cursorRow = 0;
        repaint();
        return;
    }

    if (m_requestId != 0)
        return; // not up to date

    if (m_address > m_bytesPerRow * m_rowCount)
        SetAddress(m_address - m_bytesPerRow * m_rowCount);
    else
        SetAddress(0);
}

void MemoryWidget::PageDown()
{
    if (m_cursorRow < m_rowCount - 1)
    {
        m_cursorRow = m_rowCount - 1;
        repaint();
        return;
    }

    if (m_requestId != 0)
        return; // not up to date

    SetAddress(m_address + m_bytesPerRow * m_rowCount);
}

void MemoryWidget::memoryChangedSlot(int memorySlot, uint64_t commandId)
{
    if (memorySlot != m_memSlot)
        return;

    // ignore out-of-date requests
    if (commandId != m_requestId)
        return;

    RecalcText();
}

void MemoryWidget::RecalcText()
{
    m_rows.clear();
    const Memory* pMem = m_pTargetModel->GetMemory(m_memSlot);
    if (!pMem)
        return;

    if (pMem->GetAddress() != m_address)
        return;

    // We should just save the memory block here and format on demand
    // Build up the text area
    uint32_t rowCount = m_rowCount;
    uint32_t offset = 0;
    std::vector<uint8_t> rowData;
    rowData.resize(m_bytesPerRow);

    for (uint32_t r = 0; r < rowCount; ++r)
    {
        Row row;
        for (uint32_t i = 0; i < m_bytesPerRow; ++i)
        {
            if (offset == pMem->GetSize())
            {
                if (i != 0)
                    m_rows.push_back(row);      // add unfinished row
                break;
            }

            uint8_t c = pMem->Get(offset);
            rowData[i] = c;

            row.m_hexText += QString::asprintf("%02x", rowData[i]);

            if (c >= 32 && c < 128)
                row.m_asciiText += QString::asprintf("%c", c);
            else
                row.m_asciiText += ".";
            ++offset;
        }
        m_rows.push_back(row);
    }

    m_requestId = 0;    // flag request is complete
    repaint();
}

void MemoryWidget::startStopChangedSlot()
{
    // Request new memory for the view
    if (!m_pTargetModel->IsRunning())
    {
        // Recalc a locked expression
        uint32_t addr;
        if (m_isLocked)
        {
            if (StringParsers::ParseExpression(m_addressExpression.c_str(), addr,
                                                m_pTargetModel->GetSymbolTable(),
                                                m_pTargetModel->GetRegs()))
            {
                SetAddress(addr);
            }
        }

        RequestMemory();
    }
}

void MemoryWidget::connectChangedSlot()
{
    m_rows.clear();
    m_address = 0;
    m_rowCount = 10;
    repaint();
}

void MemoryWidget::paintEvent(QPaintEvent* ev)
{
    QWidget::paintEvent(ev);

    // CAREFUL! This could lead to an infinite loop of redraws if we are not.
    RecalcRowCount();

    if (m_rows.size() == 0)
        return;

    QPainter painter(this);
    painter.setFont(monoFont);
    QFontMetrics info(painter.fontMetrics());
    const QPalette& pal = this->palette();


//    painter.setBackground(row == m_cursorRow ?
//        pal.highlight().color() :
//        pal.window().color());


    int y_base = info.ascent();
    int char_width = info.horizontalAdvance("0");

    // Set up the rendering info
    m_charWidth = char_width;

    painter.setPen(pal.text().color());
    for (size_t row = 0; row < m_rows.size(); ++row)
    {
        // Draw address string
        painter.setPen(pal.text().color());
        int y = y_base + row * m_lineHeight;       // compensate for descenders TODO use ascent()
        QString addr = QString::asprintf("%08x", m_address + m_bytesPerRow * row);
        painter.drawText(GetAddrX(), y, addr);

        // Now hex
        // We write out the values per-nybble
        const Row& r = m_rows[row];
        for (size_t i = 0; i < m_xpos.size(); ++i)
        {
            //size_t byteOffset = i / 2;
            //uint32_t shiftDown = (i % 2) * 4;

            int x = GetHexCharX(m_xpos[i]);
            QChar st = r.m_hexText.at(i);
            painter.drawText(x, y, st);
        }

        int x_ascii = GetAsciiCharX();
        painter.drawText(x_ascii, y, m_rows[row].m_asciiText);
    }

    // Draw highlight/cursor area in the hex
    {
        int y_curs = m_cursorRow * m_lineHeight;       // compensate for descenders TODO use ascent()
        int x_curs = GetHexCharX(m_xpos[m_cursorCol]);

        painter.setBrush(pal.highlight());
        painter.drawRect(x_curs, y_curs, char_width, m_lineHeight);

        int x = GetHexCharX(m_xpos[m_cursorCol]);
        QChar st = m_rows[m_cursorRow].m_hexText.at(m_cursorCol);
        painter.setPen(pal.highlightedText().color());
        painter.drawText(x_curs, y_base + y_curs, st);
    }
}

void MemoryWidget::keyPressEvent(QKeyEvent* event)
{
    switch (event->key())
    {
    case Qt::Key_Left:       MoveLeft();          return;
    case Qt::Key_Right:      MoveRight();         return;
    case Qt::Key_Up:         MoveUp();            return;
    case Qt::Key_Down:       MoveDown();          return;
    case Qt::Key_PageUp:     PageUp();            return;
    case Qt::Key_PageDown:   PageDown();          return;
    default: break;
    }
    QWidget::keyPressEvent(event);
}

void MemoryWidget::RequestMemory()
{
    uint32_t size = ((m_rowCount * 16));
    if (m_pTargetModel->IsConnected())
        m_requestId = m_pDispatcher->RequestMemory(m_memSlot, m_address, size);
}

void MemoryWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    RecalcRowCount();
}

void MemoryWidget::RecalcRowCount()
{
    // It seems that viewport is updated without this even being called,
    // which means that on startup, "h" == 0.
    int h = this->size().height();
    int rowh = m_lineHeight;
    if (rowh != 0)
        this->SetRowCount(h / rowh);
}

void MemoryWidget::RecalcSizes()
{
    monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    QFontMetrics info(monoFont);
    m_lineHeight = info.lineSpacing();
}

// Position in pixels of address column
int MemoryWidget::GetAddrX() const
{
    return 10;
}

int MemoryWidget::GetHexCharX(int x) const
{
    return GetAddrX() + (10 + x) * m_charWidth;
}

int MemoryWidget::GetAsciiCharX() const
{
    uint32_t lastCharX = m_xpos.back();
    return GetAddrX() + (10 + lastCharX + 3) * m_charWidth;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
#if 0
MemoryTableView::MemoryTableView(QWidget* parent, MemoryWidget* pModel, TargetModel* pTargetModel) :
    QTableView(parent),
    m_pTableModel(pModel),
    //m_rightClickMenu(this),
    m_rightClickRow(-1)
{
    // Actions for right-click menu
    //m_pRunUntilAction = new QAction(tr("Run to here"), this);
    //connect(m_pRunUntilAction, &QAction::triggered, this, &MemoryTableView::runToCursorRightClick);
    //m_pBreakpointAction = new QAction(tr("Toggle Breakpoint"), this);
    //m_rightClickMenu.addAction(m_pRunUntilAction);
    //m_rightClickMenu.addAction(m_pBreakpointAction);
    //new QShortcut(QKeySequence(tr("F3", "Run to cursor")),        this, SLOT(runToCursor()));
    //new QShortcut(QKeySequence(tr("F9", "Toggle breakpoint")),    this, SLOT(toggleBreakpoint()));

    //connect(m_pBreakpointAction, &QAction::triggered,                  this, &MemoryTableView::toggleBreakpointRightClick);
    connect(pTargetModel,        &TargetModel::startStopChangedSignal, this, &MemoryTableView::RecalcRowCount);

    // This table gets the focus from the parent docking widget
    setFocus();
}

/*
void MemoryTableView::contextMenuEvent(QContextMenuEvent *event)
{
    QModelIndex index = this->indexAt(event->pos());
    if (!index.isValid())
        return;

    m_rightClickRow = index.row();
    m_rightClickMenu.exec(event->globalPos());

}

void MemoryTableView::runToCursorRightClick()
{
    m_pTableModel->RunToRow(m_rightClickRow);
    m_rightClickRow = -1;
}

void MemoryTableView::toggleBreakpointRightClick()
{
    m_pTableModel->ToggleBreakpoint(m_rightClickRow);
    m_rightClickRow = -1;
}

void MemoryTableView::runToCursor()
{
    // How do we get the selected row
    QModelIndex i = this->currentIndex();
    m_pTableModel->RunToRow(i.row());
}

void MemoryTableView::toggleBreakpoint()
{
    // How do we get the selected row
    QModelIndex i = this->currentIndex();
    m_pTableModel->ToggleBreakpoint(i.row());
}
*/
QModelIndex MemoryTableView::moveCursor(QAbstractItemView::CursorAction cursorAction, Qt::KeyboardModifiers modifiers)
{
    QModelIndex i = this->currentIndex();

    // Do the override/refill behaviour if we need to scroll our virtual area
    if (cursorAction == QAbstractItemView::CursorAction::MoveUp &&
        i.row() == 0)
    {
        m_pTableModel->MoveUp();
        return i;
    }
    else if (cursorAction == QAbstractItemView::CursorAction::MoveDown &&
             i.row() >= m_pTableModel->GetRowCount() - 1)
    {
        m_pTableModel->MoveDown();
        return i;
    }
    else if (cursorAction == QAbstractItemView::CursorAction::MovePageUp &&
             i.row() == 0)
    {
        m_pTableModel->PageUp();
        return i;
    }
    else if (cursorAction == QAbstractItemView::CursorAction::MovePageDown &&
             i.row() >= m_pTableModel->GetRowCount() - 1)
    {
        m_pTableModel->PageDown();
        return i;
    }
    return QTableView::moveCursor(cursorAction, modifiers);
}

#endif

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
MemoryViewWidget::MemoryViewWidget(QWidget *parent, TargetModel* pTargetModel, Dispatcher* pDispatcher, int windowIndex) :
    QDockWidget(parent),
    m_pTargetModel(pTargetModel),
    m_pDispatcher(pDispatcher),
    m_windowIndex(windowIndex)
{
    this->setWindowTitle(QString::asprintf("Memory %d", windowIndex + 1));

    // Make the data first
    pModel = new MemoryWidget(this, pTargetModel, pDispatcher, windowIndex);
    pModel->setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));

    m_pComboBox = new QComboBox(this);
    m_pComboBox->insertItem(MemoryWidget::kModeByte, "Byte");
    m_pComboBox->insertItem(MemoryWidget::kModeWord, "Word");
    m_pComboBox->insertItem(MemoryWidget::kModeLong, "Long");
    m_pComboBox->setCurrentIndex(pModel->GetMode());

    m_pLockCheckBox = new QCheckBox(tr("Lock"), this);

    m_pLineEdit = new QLineEdit(this);
    m_pSymbolTableModel = new SymbolTableModel(this, m_pTargetModel->GetSymbolTable());
    QCompleter* pCompl = new QCompleter(m_pSymbolTableModel, this);
    pCompl->setCaseSensitivity(Qt::CaseSensitivity::CaseInsensitive);

    m_pLineEdit->setCompleter(pCompl);

    // Layouts
    QVBoxLayout* pMainLayout = new QVBoxLayout;
    QHBoxLayout* pTopLayout = new QHBoxLayout;
    auto pMainRegion = new QWidget(this);   // whole panel
    auto pTopRegion = new QWidget(this);      // top buttons/edits

    pTopLayout->addWidget(m_pLineEdit);
    pTopLayout->addWidget(m_pLockCheckBox);
    pTopLayout->addWidget(m_pComboBox);

    pMainLayout->addWidget(pTopRegion);
    pMainLayout->addWidget(pModel);

    pTopRegion->setLayout(pTopLayout);
    pMainRegion->setLayout(pMainLayout);
    setWidget(pMainRegion);

    // Listen for start/stop, so we can update our memory request
    connect(m_pLineEdit, &QLineEdit::returnPressed,         this, &MemoryViewWidget::textEditChangedSlot);
    connect(m_pLockCheckBox, &QCheckBox::stateChanged,      this, &MemoryViewWidget::lockChangedSlot);
    connect(m_pComboBox, SIGNAL(currentIndexChanged(int)),  SLOT(modeComboBoxChanged(int)));
}

void MemoryViewWidget::requestAddress(int windowIndex, bool isMemory, uint32_t address)
{
    if (!isMemory)
        return;

    if (windowIndex != m_windowIndex)
        return;

    pModel->SetLock(false);
    pModel->SetAddress(std::to_string(address));
    m_pLockCheckBox->setChecked(false);
    setVisible(true);
}

void MemoryViewWidget::textEditChangedSlot()
{
    pModel->SetAddress(m_pLineEdit->text().toStdString());
}

void MemoryViewWidget::lockChangedSlot()
{
    pModel->SetLock(m_pLockCheckBox->isChecked());
}

void MemoryViewWidget::modeComboBoxChanged(int index)
{
    pModel->SetMode((MemoryWidget::Mode)index);
    //m_pTableView->resizeColumnToContents(MemoryWidget::kColData);
}
