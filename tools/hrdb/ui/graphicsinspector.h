#ifndef GRAPHICSINSPECTOR_H
#define GRAPHICSINSPECTOR_H

#include <QDockWidget>
#include <QObject>

// Forward declarations
class QLabel;
class QLineEdit;
class QAbstractItemModel;
class QSpinBox;
class QCheckBox;
class QComboBox;

class TargetModel;
class Dispatcher;

// Taken from https://forum.qt.io/topic/94996/qlabel-and-image-antialiasing/5
class NonAntiAliasImage : public QWidget
{
    Q_OBJECT
    Q_DISABLE_COPY(NonAntiAliasImage)
public:
    virtual ~NonAntiAliasImage() override;

    explicit NonAntiAliasImage(QWidget* parent = Q_NULLPTR);
    void setPixmap(int width, int height);
    uint8_t* AllocBitmap(int size);

    QVector<QRgb>   m_colours;

    const QString& GetString() { return m_infoString; }
signals:
    void StringChanged();

protected:
    virtual void paintEvent(QPaintEvent*) override;
    virtual void mouseMoveEvent(QMouseEvent *event) override;
private:
    void UpdateString();

    QPixmap m_pixmap;
    QPointF m_mousePos;

    // Underlying bitmap data
    uint8_t* m_pBitmap;
    int m_bitmapSize;
    QString m_infoString;

};

class GraphicsInspectorWidget : public QDockWidget
{
    Q_OBJECT
public:
    GraphicsInspectorWidget(QWidget *parent,
                            TargetModel* pTargetModel, Dispatcher* pDispatcher);
    ~GraphicsInspectorWidget();

    // Grab focus and point to the main widget
    void keyFocus();
    void loadSettings();
    void saveSettings();
private:
    void connectChangedSlot();
    void startStopChangedSlot();
    void memoryChangedSlot(int memorySlot, uint64_t commandId);
    void otherMemoryChangedSlot(uint32_t address, uint32_t size);
    void textEditChangedSlot();
    void lockToVideoChangedSlot();

private slots:
    void modeChangedSlot(int index);
    void widthChangedSlot(int width);
    void heightChangedSlot(int height);
    void StringChangedSlot();
protected:
    virtual void keyPressEvent(QKeyEvent *ev);

private:
    enum Mode
    {
        k4Bitplane,
        k2Bitplane,
        k1Bitplane,
    };

    void UpdateUIElements();
    void RequestMemory();
    bool SetAddressFromVideo();
    void DisplayAddress();

    // Get the effective data by checking the "lock to" flags and
    // using them if necessary.
    GraphicsInspectorWidget::Mode GetEffectiveMode() const;
    int GetEffectiveWidth() const;
    int GetEffectiveHeight() const;

    static int32_t BytesPerMode(Mode mode);

    QLineEdit*      m_pLineEdit;
    QComboBox*      m_pModeComboBox;
    QSpinBox*       m_pWidthSpinBox;
    QSpinBox*       m_pHeightSpinBox;
    QCheckBox*      m_pLockToVideoCheckBox;
    QLabel*         m_pInfoLabel;

    NonAntiAliasImage*         m_pImageWidget;

    TargetModel*    m_pTargetModel;
    Dispatcher*     m_pDispatcher;
    QAbstractItemModel* m_pSymbolTableModel;

    Mode            m_mode;
    uint32_t        m_address;
    int             m_width;
    int             m_height;

    uint64_t        m_requestIdBitmap;
    uint64_t        m_requestIdVideoRegs;
};

#endif // GRAPHICSINSPECTOR_H
