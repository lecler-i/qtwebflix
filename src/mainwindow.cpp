#include <QContextMenuEvent>
#include <QDebug>
#include <QSettings>
#include <QStandardPaths>
#include <QWebEngineFullScreenRequest>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineView>
#include <QWidget>

#include "amazonmprisinterface.h"
#include "commandlineparser.h"
#include "defaultmprisinterface.h"
#include "mainwindow.h"
#include "mprisinterface.h"
#include "netflixmprisinterface.h"
#include "ui_mainwindow.h"
#include "urlrequestinterceptor.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow),
      mprisType(typeid(DefaultMprisInterface)),
      mpris(new DefaultMprisInterface) {
  QWebEngineSettings::globalSettings()->setAttribute(
      QWebEngineSettings::PluginsEnabled, true);
  stateSettings = new QSettings("Qtwebflix", "Save State", this);
  appSettings = new QSettings("Qtwebflix", "qtwebflix", this);
  QWebEngineProfile::defaultProfile()->setPersistentCookiesPolicy(
      QWebEngineProfile::ForcePersistentCookies);

  // set playbackrate and read jquery file
  playRate = 1.0;
  playRateStr = QString::number(playRate);
  QFile file;
  file.setFileName(":/jquery.min.js");
  file.open(QIODevice::ReadOnly);
  jQuery = file.readAll();
  jQuery.append("\nvar qt = { 'jQuery': jQuery.noConflict(true) };");
  file.close();

  ui->setupUi(this);
  this->setWindowTitle("QtWebFlix");
  readSettings();
  webview = new QWebEngineView;
  ui->horizontalLayout->addWidget(webview);

  if (appSettings->value("site").toString() == "") {
    webview->setUrl(QUrl(QStringLiteral("https://netflix.com")));
  } else {
    webview->setUrl(QUrl(stateSettings->value("site").toString()));
  }
  webview->settings()->setAttribute(
      QWebEngineSettings::FullScreenSupportEnabled, true);
// Check for QT if equal or greater than 5.10 hide scrollbars
#if HAS_SCROLLBAR
  webview->settings()->setAttribute(QWebEngineSettings::ShowScrollBars, false);
#endif

  // connect handler for fullscreen press on video
  connect(webview->page(), &QWebEnginePage::fullScreenRequested, this,
          &MainWindow::fullScreenRequested);

  m_actions["fullscreen-toggle"] =
      std::make_pair(this, SLOT(slotShortcutF11()));
  m_actions["quit"] = std::make_pair(this, SLOT(slotShortcutCtrlQ()));
  m_actions["speed-up"] = std::make_pair(this, SLOT(slotShortcutCtrlW()));
  m_actions["speed-down"] = std::make_pair(this, SLOT(slotShortcutCtrlS()));
  m_actions["speed-default"] = std::make_pair(this, SLOT(slotShortcutCtrlR()));
  m_actions["reload"] = std::make_pair(this, SLOT(slotShortcutCtrlF5()));

  // default key shortcuts
  addShortcut("fullscreen-toggle", "F11");
  addShortcut("quit", "Ctrl+Q");
  addShortcut("speed-up", "Ctrl+W");
  addShortcut("speed-down", "Ctrl+S");
  addShortcut("speed-default", "Ctrl+R");
  addShortcut("reload", "Ctrl+F5");
  appSettings->beginGroup("keybinds");
  for (auto action : appSettings->allKeys()) {
    auto keySequence = appSettings->value(action).toStringList().join(',');
    for (auto key :
         keySequence.split(QRegExp("\\s+"), QString::SkipEmptyParts)) {
      addShortcut(action, key);
    }
  }
  appSettings->endGroup();

  // Connect finished loading boolean
  connect(webview, &QWebEngineView::loadFinished, this,
          &MainWindow::finishLoading);

  // Window size settings
  QSettings settings;
  restoreState(settings.value("mainWindowState").toByteArray());

  webview->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(webview, SIGNAL(customContextMenuRequested(const QPoint &)), this,
          SLOT(ShowContextMenu(const QPoint &)));

  mpris->setup(this);
}

MainWindow::~MainWindow() {
  delete ui;
  // qDeleteAll(m_shortcuts);
}

// Slot handler of F11
void MainWindow::slotShortcutF11() {
  /* This handler will make switching applications in full screen mode
   * and back to normal window mode
   * */
  this->setFullScreen(!this->isFullScreen());
}

QWebEngineView *MainWindow::webView() const { return webview; }

// Slot handler for Ctrl + Q
void MainWindow::slotShortcutCtrlQ() {
  writeSettings();
  QApplication::quit();
}

void MainWindow::finishLoading(bool) { exchangeMprisInterfaceIfNeeded(); }

void MainWindow::addShortcut(const QString &actionName, const QString &key) {
  qDebug() << "binding " << key << " -> " << actionName;

  QSet<const QShortcut *> &shortcuts = m_shortcuts[actionName];
  auto shortcut = new QShortcut(key, this);
  if (!shortcuts.contains(shortcut)) {
    shortcuts.insert(shortcut);
  }
}

void MainWindow::registerMprisShortcutActions() {
  m_actions["play"] = std::make_pair(mpris.get(), SLOT(playVideo()));
  m_actions["pause"] = std::make_pair(mpris.get(), SLOT(pauseVideo()));
  m_actions["play-pause"] =
      std::make_pair(mpris.get(), SLOT(togglePlayPause()));
  m_actions["next-episode"] =
      std::make_pair(mpris.get(), SLOT(goNextEpisode()));
  m_actions["seek-next"] = std::make_pair(mpris.get(), SLOT(setSeek(10)));
  m_actions["seek-prev"] = std::make_pair(mpris.get(), SLOT(setSeek(-10)));

  qDebug() << "Rebinding keys because mprisChanged";
  std::for_each(
      m_shortcuts.begin(), m_shortcuts.end(),
      [&](const std::pair<QString, QSet<const QShortcut *>> &shortcutDef) {
        for (const auto &shortcut : shortcutDef.second) {
          disconnect(shortcut, SIGNAL(activated()), 0, 0);
          const auto &action = m_actions[shortcutDef.first];
          connect(shortcut, SIGNAL(activated()), action.first, action.second);
        }
      });
}

void MainWindow::exchangeMprisInterfaceIfNeeded() {
  QString hostname = webview->url().host();
  if (hostname.endsWith("netflix.com")) {
    setMprisInterface<NetflixMprisInterface>();
  } else if (hostname.endsWith("amazon.com")) {
    // use javascript to change useragent to watch HD Amazon Prime Videos as
    // using QT crashes the program.
    QString code =
        "window.navigator.__defineGetter__('userAgent', function () {"
        "return 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/68.0.3440.84 Safari/537.36 "
        "OPR/55.0.2994.34 (Edition beta)';"
        "});";

    webView()->page()->runJavaScript(code);

    setMprisInterface<AmazonMprisInterface>();
  } else {
    setMprisInterface<DefaultMprisInterface>();
  }
}

// Slot handler for Ctrl + W
void MainWindow::slotShortcutCtrlW() {
  QString getPlayer =
      ("var netflix = document.getElementsByClassName('ellipsize-text')[0];");
  webview->page()->runJavaScript(getPlayer);
  if (playRate >= 2) {
    return;
  }
  playRate += .1;
  playRateStr = QString::number(playRate);
  // QString code = QStringLiteral("qt.jQuery('video').get(0).playbackRate =")
  QString code =
      QStringLiteral("document.querySelector('video').playbackRate = ")
          .append(playRateStr);
  QString setSpeedText = QStringLiteral("var y = document.createTextNode('")
                             .append(playRateStr)
                             .append(" X');");

  QString replaceText = ("netflix.replaceChild(y, netflix.childNodes[3])");
  QString addTextToPlayer = ("netflix.appendChild(y);");
  QString addTextCode = (setSpeedText + addTextToPlayer + replaceText);
  qDebug() << "Player Speed set to: " << playRateStr;
  webview->page()->runJavaScript(code);
  webview->page()->runJavaScript(addTextCode);
}

// Slot handler for Ctrl + S
void MainWindow::slotShortcutCtrlS() {

  QString getPlayer =
      ("var netflix = document.getElementsByClassName('ellipsize-text')[0];");
  webview->page()->runJavaScript(getPlayer);
  if (playRate < 0.2) {
    return;
  }
  playRate -= .1;
  playRateStr = QString::number(playRate);
  QString code =
      QStringLiteral("document.querySelector('video').playbackRate = ")
          .append(playRateStr);
  QString setSpeedText = QStringLiteral("var y = document.createTextNode('")
                             .append(playRateStr)
                             .append(" X');");

  QString replaceText = ("netflix.replaceChild(y, netflix.childNodes[3])");
  QString addTextToPlayer = ("netflix.appendChild(y);");
  QString addTextCode = (setSpeedText + addTextToPlayer + replaceText);
  qDebug() << "Player Speed set to: " << playRateStr;
  webview->page()->runJavaScript(code);
  webview->page()->runJavaScript(addTextCode);
}

// Slot handler for Ctrl + R
void MainWindow::slotShortcutCtrlR() {
  webview->page()->runJavaScript(jQuery);
  if (playRate != 1.0) {
    playRate = 1.0;
    playRateStr = QString::number(playRate);
    QString code = QStringLiteral("qt.jQuery('video').get(0).playbackRate =")
                       .append(playRateStr);
    QString setSpeedText = QStringLiteral("var y = document.createTextNode('")
                               .append(playRateStr)
                               .append(" X');");

    QString replaceText = ("netflix.replaceChild(y, netflix.childNodes[3])");
    QString addTextToPlayer = ("netflix.appendChild(y);");
    QString addTextCode = (setSpeedText + addTextToPlayer + replaceText);
    qDebug() << "Player Speed set to: " << playRateStr;
    webview->page()->runJavaScript(code);
    webview->page()->runJavaScript(addTextCode);
  }
}

void MainWindow::slotShortcutCtrlF5() {
  webview->triggerPageAction(QWebEnginePage::ReloadAndBypassCache);
}

void MainWindow::setFullScreen(bool fullscreen) {
  if (!fullscreen) {
    this->showNormal();
  } else {
    this->showFullScreen();
  }
  mpris->updatePlayerFullScreen();
}

void MainWindow::closeEvent(QCloseEvent *) {
  // This will be called whenever this window is closed.
  writeSettings();
}

void MainWindow::writeSettings() {
  // Write the values to disk in categories.
  stateSettings->setValue("state/mainWindowState", saveState());
  stateSettings->setValue("geometry/mainWindowGeometry", saveGeometry());
  QString site = webview->url().toString();
  stateSettings->setValue("site", site);
  qDebug() << " write settings:" << site;
}

void MainWindow::restore() {

  QByteArray stateData =
      stateSettings->value("state/mainWindowState").toByteArray();

  QByteArray geometryData =
      stateSettings->value("geometry/mainWindowGeometry").toByteArray();

  restoreState(stateData);
  restoreGeometry(geometryData);
}

void MainWindow::createContextMenu(const QStringList &keys) {
  appSettings->beginGroup("providers");
  for (const auto &i : keys) {
    if (!i.startsWith("#")) {
      auto url = appSettings->value(i).toUrl();
      contextMenu.addAction(i, [this, url]() {
        qDebug() << "Switching to : " << url;
        webview->setUrl(QUrl(url));
      });
      contextMenu.addSeparator();
    }
  }
  appSettings->endGroup();
}

void MainWindow::readSettings() {
  appSettings->beginGroup("providers");
  QStringList providers = appSettings->allKeys();

  // Check if config file exists,if not create a default key.
  if (!providers.size()) {
    qDebug() << "Config file does not exist, creating default";
    appSettings->setValue("netflix", "http://netflix.com");
    appSettings->sync();
    providers = appSettings->allKeys();
  }
  appSettings->endGroup();
  createContextMenu(providers);

  restore();
}

void MainWindow::fullScreenRequested(QWebEngineFullScreenRequest request) {

  // fullscreen on video players

  if (request.toggleOn() && !this->isFullScreen()) {
    this->showFullScreen();
  } else {
    this->showNormal();
  }
  mpris->updatePlayerFullScreen();
  request.accept();
}

void MainWindow::ShowContextMenu(const QPoint &pos) // this is a slot
{
  QPoint globalPos = webview->mapToGlobal(pos);
  contextMenu.exec(globalPos);
}

void MainWindow::parseCommand() {
  // create parser object and get arguemts
  Commandlineparser parser;

  // check if argument is used and set provider
  if (parser.providerIsSet()) {
    if (parser.getProvider() == "") {
      qDebug() << "site is invalid reditecting to netflix.com";
      webview->setUrl(QUrl(QStringLiteral("https://netflix.com")));
    } else if (parser.getProvider() != "") {
      qDebug() << "site is set to" << parser.getProvider();
      webview->setUrl(QUrl::fromUserInput(parser.getProvider()));
    }
  }

  // check if argument is used and set useragent
  if (parser.userAgentisSet()) {
    qDebug() << "Changing useragent to :" << parser.getUserAgent();
    this->webview->page()->profile()->setHttpUserAgent(parser.getUserAgent());
  }
  if (!parser.nonHDisSet()) {
    this->m_interceptor = new UrlRequestInterceptor;
    this->webview->page()->profile()->setRequestInterceptor(
        this->m_interceptor);
  }
}
