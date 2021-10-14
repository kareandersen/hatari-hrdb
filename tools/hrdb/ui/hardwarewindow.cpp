#include "hardwarewindow.h"

#include <QLineEdit>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QSettings>
#include <QDebug>
#include <QLabel>
#include <QScrollArea>

#include "../transport/dispatcher.h"
#include "../models/targetmodel.h"
#include "../models/session.h"
#include "../hardware/hardware_st.h"
#include "../hardware/regs_st.h"
#include "quicklayout.h"

//-----------------------------------------------------------------------------
bool GetField(const Memory& mem, const Regs::FieldDef& def, QString& result)
{
    if (!mem.HasAddress(def.regAddr))
        return false;

    uint8_t regVal = mem.ReadAddressByte(def.regAddr);
    uint8_t extracted = (regVal >> def.shift) & def.mask;
    if (def.strings)
        result = GetString(def.strings, extracted);
    else {
        result = QString::asprintf("%u ($%x)", extracted, extracted);
    }
    return true;
}

//-----------------------------------------------------------------------------
class HardwareField
{
public:
    HardwareField()
    {
        m_pDefaultLabel = new QLabel();
        m_pDefaultLabel->setTextInteractionFlags(Qt::TextInteractionFlag::TextSelectableByMouse);
    }

    virtual ~HardwareField();
    virtual bool Update(const TargetModel* pTarget) = 0;
    virtual QWidget* GetWidget() = 0;
    virtual void UpdateSettings(const Session::Settings& settings);

protected:
    QLabel*  m_pDefaultLabel;
};

//-----------------------------------------------------------------------------
HardwareField::~HardwareField()
{
    delete m_pDefaultLabel;
}

//-----------------------------------------------------------------------------
void HardwareField::UpdateSettings(const Session::Settings &settings)
{
    m_pDefaultLabel->setFont(settings.m_font);
}

//-----------------------------------------------------------------------------
class HardwareFieldRegEnum : public HardwareField
{
public:
    HardwareFieldRegEnum(const Regs::FieldDef& def) :
        m_def(def)
    {
    }

    bool Update(const TargetModel* pTarget);
    virtual QWidget* GetWidget() { return m_pDefaultLabel; }

    const Regs::FieldDef&   m_def;
    QString                 m_lastVal;
};

//-----------------------------------------------------------------------------
class HardwareFieldBitmask : public HardwareField
{
public:
    HardwareFieldBitmask(const Regs::FieldDef** defs) :
        m_pDefs(defs)
    {
    }

    bool Update(const TargetModel* pTarget);
    virtual QWidget* GetWidget() { return m_pDefaultLabel; }

    const Regs::FieldDef**  m_pDefs;
};

//-----------------------------------------------------------------------------
class HardwareFieldRegScreenbase : public HardwareField
{
public:
    HardwareFieldRegScreenbase()
    {
    }

    bool Update(const TargetModel* pTarget);
    virtual QWidget* GetWidget() { return m_pDefaultLabel; }
};

//-----------------------------------------------------------------------------
class HardwareFieldYm : public HardwareField
{
public:
    HardwareFieldYm(int index) :
        m_index(index)
    {
    }

    bool Update(const TargetModel* pTarget);
    virtual QWidget* GetWidget() { return m_pDefaultLabel; }
    int m_index;
};

//-----------------------------------------------------------------------------
class HardwareFieldYmPeriod : public HardwareField
{
public:
    HardwareFieldYmPeriod(int index) :
        m_index(index)
    {
    }

    bool Update(const TargetModel* pTarget);
    virtual QWidget* GetWidget() { return m_pDefaultLabel; }
    int m_index;
};

//-----------------------------------------------------------------------------
class HardwareFieldYmEnvShape : public HardwareField
{
public:
    HardwareFieldYmEnvShape() {}

    bool Update(const TargetModel* pTarget);
    virtual QWidget* GetWidget() { return m_pDefaultLabel; }
};

//-----------------------------------------------------------------------------
class HardwareFieldYmMixer : public HardwareField
{
public:
    HardwareFieldYmMixer() {}

    bool Update(const TargetModel* pTarget);
    virtual QWidget* GetWidget() { return m_pDefaultLabel; }
};

//-----------------------------------------------------------------------------
class HardwareFieldYmVolume : public HardwareField
{
public:
    HardwareFieldYmVolume(int index) :
        m_index(index)
    {
    }

    bool Update(const TargetModel* pTarget);
    virtual QWidget* GetWidget() { return m_pDefaultLabel; }
    int m_index;
};

//-----------------------------------------------------------------------------
HardwareWindow::HardwareWindow(QWidget *parent, Session* pSession) :
    QDockWidget(parent),
    m_pSession(pSession),
    m_pTargetModel(pSession->m_pTargetModel),
    m_pDispatcher(pSession->m_pDispatcher),
    m_videoMem(0, 0),
    m_mfpMem(0, 0)
{
    this->setWindowTitle("Hardware");
    setObjectName("Hardware");

    //m_pTableView = new QTreeView(this);
    //m_pTableView->setModel(new HardwareTableModel(this, m_pTargetModel, m_pDispatcher));

    // Layouts
    QVBoxLayout* pMainLayout = new QVBoxLayout(this);
    pMainLayout->setSizeConstraint(QLayout::SetFixedSize);  // constraints children to minimums sizes so they "stack"
    SetMargins(pMainLayout);

    auto pMainRegion = new QWidget(this);   // whole panel

    //SetMargins(pTopLayout);
    //pTopLayout->addWidget(m_pTableView);
    // Make each row a set of widgets?

    Expander* pExpMmu = new Expander(this, "MMU - Memory Management Unit");
    addField(pExpMmu->m_pBottomLayout,  "Bank 0",                   Regs::g_fieldDef_MMU_CONFIG_BANK0);
    addField(pExpMmu->m_pBottomLayout,  "Bank 1",                   Regs::g_fieldDef_MMU_CONFIG_BANK1);

    Expander* pExpVideo = new Expander(this, "Shifter/Glue - Video");

    addField(pExpVideo->m_pBottomLayout,  "Resolution",             Regs::g_fieldDef_VID_SHIFTER_RES_RES);
    addField(pExpVideo->m_pBottomLayout,  "Sync Rate",              Regs::g_fieldDef_VID_SYNC_MODE_RATE);
    addShared(pExpVideo->m_pBottomLayout, "Video Base",             new HardwareFieldRegScreenbase());

    addField(pExpVideo->m_pBottomLayout, "Horizontal Scroll (STE)", Regs::g_fieldDef_VID_HORIZ_SCROLL_STE_PIXELS);
    addField(pExpVideo->m_pBottomLayout, "Scanline offset (STE)",   Regs::g_fieldDef_VID_SCANLINE_OFFSET_STE_ALL);

    Expander* pExpMfp = new Expander(this, "MFP 68901 - Multi-Function Peripheral");

    addField(pExpMfp->m_pBottomLayout, "Parallel Port Data",        Regs::g_fieldDef_MFP_GPIP_ALL);
    addBitmask(pExpMfp->m_pBottomLayout, "Active Edge low->high",   Regs::g_regFieldsDef_MFP_AER);
    addBitmask(pExpMfp->m_pBottomLayout, "Data Direction output",   Regs::g_regFieldsDef_MFP_DDR);

    addBitmask(pExpMfp->m_pBottomLayout, "IERA (Enable)",           Regs::g_regFieldsDef_MFP_IERA);
    addBitmask(pExpMfp->m_pBottomLayout, "IMRA (Mask)",             Regs::g_regFieldsDef_MFP_IMRA);
    addBitmask(pExpMfp->m_pBottomLayout, "IPRA (Pending)",          Regs::g_regFieldsDef_MFP_IPRA);
    addBitmask(pExpMfp->m_pBottomLayout, "ISRA (Service)",          Regs::g_regFieldsDef_MFP_ISRA);

    addBitmask(pExpMfp->m_pBottomLayout, "IERB (Enable)",           Regs::g_regFieldsDef_MFP_IERB);
    addBitmask(pExpMfp->m_pBottomLayout, "IMRB (Mask)",             Regs::g_regFieldsDef_MFP_IMRB);
    addBitmask(pExpMfp->m_pBottomLayout, "IPRB (Pending)",          Regs::g_regFieldsDef_MFP_IPRB);
    addBitmask(pExpMfp->m_pBottomLayout, "ISRB (Service)",          Regs::g_regFieldsDef_MFP_ISRB);

    addField(pExpMfp->m_pBottomLayout, "Vector Base offset",        Regs::g_fieldDef_MFP_VR_VEC_BASE_OFFSET);
    addField(pExpMfp->m_pBottomLayout, "End-of-Interrupt",          Regs::g_fieldDef_MFP_VR_ENDINT);

    addField(pExpMfp->m_pBottomLayout, "Timer A Control Mode",      Regs::g_fieldDef_MFP_TACR_MODE_TIMER_A);
    addField(pExpMfp->m_pBottomLayout, "Timer A Data",              Regs::g_fieldDef_MFP_TADR_ALL);
    addField(pExpMfp->m_pBottomLayout, "Timer B Control Mode",      Regs::g_fieldDef_MFP_TBCR_MODE_TIMER_B);
    addField(pExpMfp->m_pBottomLayout, "Timer B Data",              Regs::g_fieldDef_MFP_TBDR_ALL);
    addField(pExpMfp->m_pBottomLayout, "Timer C Control Mode",      Regs::g_fieldDef_MFP_TCDCR_MODE_TIMER_C);
    addField(pExpMfp->m_pBottomLayout, "Timer C Data",              Regs::g_fieldDef_MFP_TCDR_ALL);
    addField(pExpMfp->m_pBottomLayout, "Timer D Control Mode",      Regs::g_fieldDef_MFP_TCDCR_MODE_TIMER_D);
    addField(pExpMfp->m_pBottomLayout, "Timer D Data",              Regs::g_fieldDef_MFP_TDDR_ALL);

    addField(pExpMfp->m_pBottomLayout, "Sync Char",                 Regs::g_fieldDef_MFP_SCR_ALL);
    addBitmask(pExpMfp->m_pBottomLayout, "USART Control",           Regs::g_regFieldsDef_MFP_UCR);
    addBitmask(pExpMfp->m_pBottomLayout, "USART RX Status",         Regs::g_regFieldsDef_MFP_RSR);
    addBitmask(pExpMfp->m_pBottomLayout, "USART TX Status",         Regs::g_regFieldsDef_MFP_TSR);
    addField(pExpMfp->m_pBottomLayout, "USART Data",                Regs::g_fieldDef_MFP_UDR_ALL);

    Expander* pExpYm = new Expander(this, "YM/PSG");
    addShared(pExpYm->m_pBottomLayout, "Period A",     new HardwareFieldYmPeriod(Regs::YM_PERIOD_A_LO));
    addShared(pExpYm->m_pBottomLayout, "Period B",     new HardwareFieldYmPeriod(Regs::YM_PERIOD_B_LO));
    addShared(pExpYm->m_pBottomLayout, "Period C",     new HardwareFieldYmPeriod(Regs::YM_PERIOD_C_LO));
    addShared(pExpYm->m_pBottomLayout, "Noise Period", new HardwareFieldYmPeriod(Regs::YM_PERIOD_NOISE));
    addShared(pExpYm->m_pBottomLayout, "Mixer",        new HardwareFieldYmMixer());
    addShared(pExpYm->m_pBottomLayout, "Volume A",     new HardwareFieldYmVolume(Regs::YM_VOLUME_A));
    addShared(pExpYm->m_pBottomLayout, "Volume B",     new HardwareFieldYmVolume(Regs::YM_VOLUME_B));
    addShared(pExpYm->m_pBottomLayout, "Volume C",     new HardwareFieldYmVolume(Regs::YM_VOLUME_C));
    addShared(pExpYm->m_pBottomLayout, "Env Period",   new HardwareFieldYmPeriod(Regs::YM_PERIOD_ENV_LO));
    addShared(pExpYm->m_pBottomLayout, "Env Shape",    new HardwareFieldYmEnvShape());
    addShared(pExpYm->m_pBottomLayout, "Port A",       new HardwareFieldYm(Regs::YM_PORT_A));
    addShared(pExpYm->m_pBottomLayout, "Port B",       new HardwareFieldYm(Regs::YM_PORT_B));

    pMainLayout->addWidget(pExpMmu);
    pMainLayout->addWidget(pExpVideo);
    pMainLayout->addWidget(pExpMfp);
    pMainLayout->addWidget(pExpYm);

    pMainRegion->setLayout(pMainLayout);
    QScrollArea* sa = new QScrollArea(this);
    sa->setWidget(pMainRegion);
    sa->setHorizontalScrollBarPolicy(Qt::ScrollBarPolicy::ScrollBarAlwaysOff);
    setWidget(sa);

    loadSettings();

    connect(m_pTargetModel,  &TargetModel::connectChangedSignal,   this, &HardwareWindow::connectChangedSlot);
    connect(m_pTargetModel,  &TargetModel::startStopChangedSignal, this, &HardwareWindow::startStopChangedSlot);
    connect(m_pTargetModel,  &TargetModel::memoryChangedSignal,    this, &HardwareWindow::memoryChangedSlot);
    connect(m_pTargetModel,  &TargetModel::ymChangedSignal,        this, &HardwareWindow::ymChangedSlot);

    connect(m_pSession,      &Session::settingsChanged,            this, &HardwareWindow::settingsChangedSlot);

    // Refresh enable state
    connectChangedSlot();

    // Refresh font
    settingsChangedSlot();
}

HardwareWindow::~HardwareWindow()
{
    qDeleteAll(m_fields);
}

void HardwareWindow::keyFocus()
{
    activateWindow();
    this->setFocus();
}

void HardwareWindow::loadSettings()
{
    QSettings settings;
    settings.beginGroup("Hardware");

    restoreGeometry(settings.value("geometry").toByteArray());
    settings.endGroup();
}

void HardwareWindow::saveSettings()
{
    QSettings settings;
    settings.beginGroup("Hardware");

    settings.setValue("geometry", saveGeometry());
    settings.endGroup();
}

void HardwareWindow::connectChangedSlot()
{
    if (m_pTargetModel->IsConnected())
    {
        if (m_pTargetModel->IsConnected())
        {
        }
    }
    else
    {
    }
}

//-----------------------------------------------------------------------------
void HardwareWindow::startStopChangedSlot()
{
    if (m_pTargetModel->IsRunning())
    {

    }
    else {
        m_pDispatcher->RequestMemory(MemorySlot::kHardwareWindow, Regs::MMU_CONFIG,    0x1);
        m_pDispatcher->RequestMemory(MemorySlot::kHardwareWindow, Regs::VID_REG_BASE,  0x70);
        m_pDispatcher->RequestMemory(MemorySlot::kHardwareWindow, Regs::MFP_GPIP,      0x30);

        m_pDispatcher->InfoYm();
    }
}

void HardwareWindow::memoryChangedSlot(int memorySlot, uint64_t /*commandId*/)
{
    if (memorySlot == MemorySlot::kHardwareWindow)
    {
        for (auto pField : m_fields)
        {
            pField->Update(m_pTargetModel);
        }
    }
}

void HardwareWindow::ymChangedSlot()
{
    for (auto pField : m_fields)
    {
        pField->Update(m_pTargetModel);
    }
}

void HardwareWindow::settingsChangedSlot()
{
    for (auto pField : m_fields)
        pField->UpdateSettings(m_pSession->GetSettings());
}

void HardwareWindow::addField(QFormLayout* pLayout, const QString& title, const Regs::FieldDef &def)
{
    HardwareFieldRegEnum* pField = new HardwareFieldRegEnum(def);
    addShared(pLayout, title, pField);
}

void HardwareWindow::addBitmask(QFormLayout* pLayout, const QString& title, const Regs::FieldDef** defs)
{
    HardwareFieldBitmask* pField = new HardwareFieldBitmask(defs);
    addShared(pLayout, title, pField);
}

void HardwareWindow::addShared(QFormLayout *pLayout, const QString &title, HardwareField *pField)
{
    m_fields.append(pField);
    QLabel* pLabel = new QLabel(this);
    pLabel->setMargin(0);
    pLabel->setText(title);
    pLabel->setTextInteractionFlags(Qt::TextInteractionFlag::TextSelectableByMouse);
    pLayout->addRow(pLabel, pField->GetWidget());
}

Expander::Expander(QWidget *parent, QString text) :
    QWidget(parent),
    m_text(text),
    m_expanded(false)
{
    m_pTop = new QWidget(parent);

    // Stop this restretching
    //this->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    m_pBottom = new QWidget(parent);
    m_pButton = new ExpandLabel(parent);
    m_pButton->setText("Press");
    m_pButton->setMinimumWidth(1000);

    m_pTopLayout = new QHBoxLayout(parent);
    m_pTopLayout->addWidget(m_pButton);
    m_pTopLayout->addStretch();

    m_pBottomLayout = new QFormLayout(parent);
    SetMarginsRows(m_pBottomLayout);
    m_pTop->setLayout(m_pTopLayout);

    m_pBottom->setLayout(m_pBottomLayout);
    m_pTop->setStyleSheet("QWidget { background: #a0a0a0; color: yellow }");

    QVBoxLayout* mainLayout = new QVBoxLayout;
    mainLayout->setMargin(0);
    mainLayout->setSpacing(0);
    mainLayout->setSizeConstraint(QLayout::SetFixedSize);  // constraints children to minimums sizes so they "stack"

    mainLayout->addWidget(m_pTop);/*, 0, Qt::AlignTop | Qt:: AlignLeft);*/
    mainLayout->addWidget(m_pBottom);/*, 0, Qt::AlignTop | Qt:: AlignLeft);*/
    this->setLayout(mainLayout);

    UpdateState();
    connect(m_pButton,  &ExpandLabel::clicked, this, &Expander::buttonPressedSlot);
}

void Expander::buttonPressedSlot()
{
    m_expanded = !m_expanded;
    UpdateState();
}

void Expander::UpdateState()
{
    m_pBottom->setVisible(m_expanded);
    if (m_expanded) {
        m_pButton->setText(QString("- ") + m_text);
    }
    else {
        m_pButton->setText(QString("+ ") + m_text);
    }
}

ExpandLabel::ExpandLabel(QWidget *parent) :
    QLabel(parent)
{
}

void ExpandLabel::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::MouseButton::LeftButton)
        emit clicked();

    return QLabel::mousePressEvent(event);
}

bool HardwareFieldRegEnum::Update(const TargetModel* pTarget)
{
    const Memory& mem = *pTarget->GetMemory(MemorySlot::kHardwareWindow);
    QString str;
    if (!GetField(mem, m_def, str))
        return false;       // Wrong memory

    if (str != m_lastVal)
        m_pDefaultLabel->setText(QString("<b>") + str + "</b>");
    else
        m_pDefaultLabel->setText(str);

    m_lastVal = str;
    return true;
}

bool HardwareFieldBitmask::Update(const TargetModel* pTarget)
{
    const Memory& mem = *pTarget->GetMemory(MemorySlot::kHardwareWindow);
    QString res;
    QTextStream ref(&res);
    const Regs::FieldDef** pDef = m_pDefs;
    for (; *pDef; ++pDef)
    {
        const Regs::FieldDef* pCurrDef = *pDef;
        if (!mem.HasAddress(pCurrDef->regAddr))
            return false;
        uint8_t regVal = mem.ReadAddressByte(pCurrDef->regAddr);
        uint16_t extracted = (regVal >> pCurrDef->shift) & pCurrDef->mask;

        if (pCurrDef->mask == 1 && !pCurrDef->strings)
        {
            // Single bit
            if (extracted)
            {
                ref << (*pDef)->name << " ";
            }
        }
        else {
            if (pCurrDef->strings)
            {
                const char* str = Regs::GetString(pCurrDef->strings, extracted);
                ref << pCurrDef->name << "=" << str << " ";
            }
            else
            {
                ref << pCurrDef->name << "=" << extracted << " ";
            }
        }
    }
    m_pDefaultLabel->setText(res);
    return true;
}

bool HardwareFieldRegScreenbase::Update(const TargetModel* pTarget)
{
    const Memory& mem = *pTarget->GetMemory(MemorySlot::kHardwareWindow);
    uint32_t address;
    if (HardwareST::GetVideoBase(mem, MACHINETYPE::MACHINE_ST, address))
    {
        m_pDefaultLabel->setText(QString::asprintf("$%x", address));
        return true;
    }
    return false;

}

bool HardwareFieldYm::Update(const TargetModel* pTarget)
{
    uint8_t val = pTarget->GetYm().m_regs[m_index];
    m_pDefaultLabel->setText(QString::asprintf("%u ($%x)", val, val));
    return true;
}

bool HardwareFieldYmPeriod::Update(const TargetModel *pTarget)
{
    uint16_t valLo = (pTarget->GetYm().m_regs[m_index]);
    uint16_t valHi = (pTarget->GetYm().m_regs[m_index + 1]);
    uint16_t mask = 0xfff;
    double clock = 2000000.0;

    // Divisors are taken from the Datasheet.
    double divisor = clock / 16;

    if (m_index == Regs::YM_PERIOD_ENV_LO)
    {
        divisor = clock / 256;
        mask = 0xffff;
    }
    else if (m_index == Regs::YM_PERIOD_NOISE)
    {
        mask = 0x1f;
    }

    // Special cases
    uint32_t val = (valHi * 256U) | valLo;
    val &= mask;

    double hertz = divisor / (val ? val : 1);
    m_pDefaultLabel->setText(QString::asprintf("$%03x  Approx %.0fHz", val, hertz));
    return true;
}

bool HardwareFieldYmEnvShape::Update(const TargetModel *pTarget)
{
    uint16_t val = (pTarget->GetYm().m_regs[Regs::YM_PERIOD_ENV_SHAPE]);
    const char* pString = Regs::GetString(static_cast<Regs::ENV_SHAPE>(val));
    m_pDefaultLabel->setText(pString);
    return true;
}

bool HardwareFieldYmMixer::Update(const TargetModel *pTarget)
{
    uint16_t val = (pTarget->GetYm().m_regs[Regs::YM_MIXER]);

    QString str;
    if (!Regs::GetField_YM_MIXER_TONE_A_OFF(val))
        str += "SQUARE_A ";
    if (!Regs::GetField_YM_MIXER_TONE_B_OFF(val))
        str += "SQUARE_B ";
    if (!Regs::GetField_YM_MIXER_TONE_C_OFF(val))
        str += "SQUARE_C ";

    if (!Regs::GetField_YM_MIXER_NOISE_A_OFF(val))
        str += "NOISE_A ";
    if (!Regs::GetField_YM_MIXER_NOISE_B_OFF(val))
        str += "NOISE_B ";
    if (!Regs::GetField_YM_MIXER_NOISE_C_OFF(val))
        str += "NOISE_C ";

    m_pDefaultLabel->setText(str);
    return true;

}

bool HardwareFieldYmVolume::Update(const TargetModel *pTarget)
{
    uint16_t val = pTarget->GetYm().m_regs[m_index];
    uint8_t squareVol = Regs::GetField_YM_VOLUME_A_VOL(val);
    bool useEnv = Regs::GetField_YM_VOLUME_A_ENVELOPE(val);

    m_pDefaultLabel->setText(QString::asprintf("Square Vol = %u%s",
                                               squareVol,
                                               useEnv ? " + ENVELOPE" : ""));
    return true;
}