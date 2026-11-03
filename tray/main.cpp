// TrueHDR Desktop tray: on/off, peak nits, AI upscaling options.
// Writes the effect's settings to kwinrc [Effect-truehdr] and asks KWin to reload them over D-Bus.

#include <KConfigGroup>
#include <KSharedConfig>

#include <QActionGroup>
#include <QApplication>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <QFileInfo>
#include <QVBoxLayout>

namespace
{
const QString kEffect = QStringLiteral("truehdr");

KConfigGroup group()
{
    static KSharedConfig::Ptr cfg = KSharedConfig::openConfig(QStringLiteral("kwinrc"));
    cfg->reparseConfiguration();
    return cfg->group(QStringLiteral("Effect-truehdr"));
}

QDBusInterface &kwin()
{
    static QDBusInterface i(QStringLiteral("org.kde.KWin"), QStringLiteral("/Effects"), QStringLiteral("org.kde.kwin.Effects"));
    return i;
}

bool loaded()
{
    QDBusReply<bool> r = kwin().call(QStringLiteral("isEffectLoaded"), kEffect);
    return r.isValid() && r.value();
}

template<typename T>
void set(const char *key, const T &value)
{
    KConfigGroup g = group();
    g.writeEntry(key, value);
    g.sync();
    if (loaded()) {
        kwin().call(QStringLiteral("reconfigureEffect"), kEffect);
    }
}

void setEnabled(bool on)
{
    KSharedConfig::Ptr cfg = KSharedConfig::openConfig(QStringLiteral("kwinrc"));
    cfg->group(QStringLiteral("Plugins")).writeEntry("truehdrEnabled", on);
    cfg->group(QStringLiteral("Effect-truehdr")).writeEntry("EnabledAtStart", true);
    cfg->sync();
    kwin().call(on ? QStringLiteral("loadEffect") : QStringLiteral("unloadEffect"), kEffect);
}

QIcon makeIcon(bool on)
{
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(on ? QColor(255, 176, 32) : QColor(128, 128, 128));
    p.drawRoundedRect(4, 10, 56, 44, 8, 8);
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(22);
    p.setFont(f);
    p.setPen(on ? Qt::black : QColor(40, 40, 40));
    p.drawText(QRect(4, 10, 56, 44), Qt::AlignCenter, QStringLiteral("HDR"));
    return QIcon(pm);
}

QString autostartFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/autostart/truehdr-tray.desktop");
}

void setAutostart(bool on)
{
    if (!on) {
        QFile::remove(autostartFile());
        return;
    }
    QDir().mkpath(QFileInfo(autostartFile()).path());
    QFile f(autostartFile());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write("[Desktop Entry]\nType=Application\nName=TrueHDR Desktop\nExec=truehdr-tray\n"
                "Icon=video-display\nX-KDE-autostart-phase=2\nNoDisplay=true\n");
    }
}

// slider + spin box, 400..2000 nits
bool askNits(int &nits)
{
    QDialog d;
    d.setWindowTitle(QStringLiteral("TrueHDR peak brightness"));
    auto *lay = new QVBoxLayout(&d);
    lay->addWidget(new QLabel(QStringLiteral("Peak brightness of highlights (nits).\nSet this to your display's real peak.")));
    auto *row = new QHBoxLayout;
    auto *slider = new QSlider(Qt::Horizontal);
    auto *spin = new QSpinBox;
    slider->setRange(400, 2000);
    spin->setRange(400, 2000);
    slider->setSingleStep(50);
    spin->setSingleStep(50);
    spin->setSuffix(QStringLiteral(" nits"));
    slider->setValue(nits);
    spin->setValue(nits);
    QObject::connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
    QObject::connect(spin, &QSpinBox::valueChanged, slider, &QSlider::setValue);
    // live preview while dragging
    QObject::connect(spin, &QSpinBox::valueChanged, [](int v) { set("Luminance", v); });
    row->addWidget(slider, 1);
    row->addWidget(spin);
    lay->addLayout(row);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    lay->addWidget(bb);
    d.resize(420, d.sizeHint().height());
    const int before = nits;
    if (d.exec() == QDialog::Accepted) {
        nits = spin->value();
        return true;
    }
    set("Luminance", before);
    return false;
}

struct Choice {
    const char *label;
    QVariant value;
};

QMenu *choiceMenu(QMenu *parent, const QString &title, const char *key, const QVariant &def, std::initializer_list<Choice> choices)
{
    QMenu *m = parent->addMenu(title);
    auto *grp = new QActionGroup(m);
    for (const Choice &c : choices) {
        QAction *a = m->addAction(QString::fromUtf8(c.label));
        a->setCheckable(true);
        a->setData(c.value);
        grp->addAction(a);
        QObject::connect(a, &QAction::triggered, [key, v = c.value] {
            if (v.typeId() == QMetaType::QString) {
                set(key, v.toString());
            } else {
                set(key, v.toInt());
            }
        });
    }
    QObject::connect(m, &QMenu::aboutToShow, [m, key, def] {
        const QString cur = group().readEntry(key, def.toString());
        for (QAction *a : m->actions()) {
            a->setChecked(a->data().toString() == cur);
        }
    });
    return m;
}
} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("truehdr-tray"));
    app.setDesktopFileName(QStringLiteral("truehdr-tray"));
    app.setQuitOnLastWindowClosed(false);

    // wait for the tray host at login (plasmashell can start after us)
    for (int i = 0; i < 30 && !QSystemTrayIcon::isSystemTrayAvailable(); ++i) {
        QThread::sleep(1);
    }

    QSystemTrayIcon tray;
    QMenu menu;

    QAction *onOff = menu.addAction(QStringLiteral("TrueHDR on"));
    onOff->setCheckable(true);
    QObject::connect(onOff, &QAction::triggered, [](bool on) { setEnabled(on); });

    QAction *nitsAct = menu.addAction(QString());
    QObject::connect(nitsAct, &QAction::triggered, [] {
        int n = group().readEntry("Luminance", 1000);
        if (askNits(n)) {
            set("Luminance", n);
        }
    });
    choiceMenu(&menu, QStringLiteral("Brightness presets"), "Luminance", 1000,
               {{"400 nits", 400}, {"600 nits", 600}, {"800 nits", 800}, {"1000 nits (default)", 1000}, {"1500 nits", 1500}, {"2000 nits", 2000}});
    menu.addSeparator();

    QAction *up = menu.addAction(QStringLiteral("AI upscaling of fullscreen video/games"));
    up->setCheckable(true);
    QObject::connect(up, &QAction::triggered, [](bool on) { set("Upscale", on); });
    choiceMenu(&menu, QStringLiteral("Upscaling quality"), "UpscaleQuality", 17,
               {{"Faithful (17, default)", 17}, {"Ultra / sharper (4)", 4}, {"High (3)", 3}, {"Fast (2)", 2}});
    choiceMenu(&menu, QStringLiteral("Block cleanup on native content"), "Cleanup", QStringLiteral("off"),
               {{"Off (default)", QStringLiteral("off")}, {"Fullscreen only", QStringLiteral("fullscreen")}, {"Always", QStringLiteral("always")}});
    QAction *deband = menu.addAction(QStringLiteral("AI debanding"));
    deband->setCheckable(true);
    QObject::connect(deband, &QAction::triggered, [](bool on) { set("DebandingOff", !on); });
    menu.addSeparator();

    QAction *reset = menu.addAction(QStringLiteral("Reset to defaults"));
    QObject::connect(reset, &QAction::triggered, [] {
        KConfigGroup g = group();
        for (const char *k : {"Contrast", "Saturation", "MiddleGray"}) {
            g.writeEntry(k, 100);
        }
        g.writeEntry("MiddleGray", 50);
        g.writeEntry("Luminance", 1000);
        g.writeEntry("DebandingOff", false);
        g.writeEntry("Upscale", true);
        g.writeEntry("UpscaleQuality", 17);
        g.writeEntry("Cleanup", QStringLiteral("off"));
        g.sync();
        set("Luminance", 1000);
    });
    QAction *autoAct = menu.addAction(QStringLiteral("Show in tray at login"));
    autoAct->setCheckable(true);
    QObject::connect(autoAct, &QAction::triggered, [](bool on) { setAutostart(on); });
    menu.addAction(QStringLiteral("Quit tray"), &app, &QApplication::quit);

    auto refresh = [&] {
        const bool on = loaded();
        const KConfigGroup g = group();
        const int nits = g.readEntry("Luminance", 1000);
        onOff->setChecked(on);
        nitsAct->setText(QStringLiteral("Peak brightness: %1 nits…").arg(nits));
        up->setChecked(g.readEntry("Upscale", true));
        deband->setChecked(!g.readEntry("DebandingOff", false));
        autoAct->setChecked(QFile::exists(autostartFile()));
        tray.setIcon(makeIcon(on));
        tray.setToolTip(on ? QStringLiteral("TrueHDR on, %1 nits").arg(nits) : QStringLiteral("TrueHDR off"));
    };
    QObject::connect(&menu, &QMenu::aboutToShow, refresh);
    QObject::connect(&tray, &QSystemTrayIcon::activated, [&](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::Trigger) { // left click toggles
            setEnabled(!loaded());
            QTimer::singleShot(300, refresh);
        }
    });
    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, refresh);
    poll.start(3000);

    tray.setContextMenu(&menu);
    refresh();
    tray.show();

    if (!QFile::exists(QDir::homePath() + QStringLiteral("/.local/share/truehdr-desktop/VideoFX/lib/libVideoFX.so"))
        && !QFile::exists(QStringLiteral("/usr/local/VideoFX/lib/libVideoFX.so"))
        && group().readEntry("SdkPath", QString()).isEmpty()) {
        tray.showMessage(QStringLiteral("TrueHDR Desktop"),
                         QStringLiteral("NVIDIA Maxine SDK not found. Run ./install.sh sdk from the download folder."),
                         QSystemTrayIcon::Warning, 10000);
    }
    return app.exec();
}
