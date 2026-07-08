#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.h>

#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkInterface>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <optional>
#include <regex>
#include <string>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("absojlut-autoblur-raspberry", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "AbsoJlut AutoBlur Raspberry: Raspberry/ReYohoho video bridge -> OBS blur filter";
}

namespace {
constexpr int kBridgePort = 8799;
constexpr const char *kPluginName = "AbsoJlut AutoBlur Raspberry";

struct Interval {
	int start = 0;
	int end = 0;
	QString raw;

	bool contains(double seconds, double preRoll, double postRoll) const
	{
		return seconds >= static_cast<double>(start) - preRoll &&
		       seconds <= static_cast<double>(end) + postRoll;
	}
};

struct Snapshot {
	double currentTime = 0.0;
	double duration = -1.0;
	bool paused = false;
	double playbackRate = 1.0;
	QString title;
	QString url;
	qint64 receivedMs = 0;
};

QString formatSeconds(double seconds)
{
	if (!std::isfinite(seconds) || seconds < 0)
		return "--:--:--";
	const int total = std::max(0, static_cast<int>(std::round(seconds)));
	const int h = total / 3600;
	const int m = (total % 3600) / 60;
	const int s = total % 60;
	return QString("%1:%2:%3")
		.arg(h, 2, 10, QLatin1Char('0'))
		.arg(m, 2, 10, QLatin1Char('0'))
		.arg(s, 2, 10, QLatin1Char('0'));
}

QString normalizeTimeText(QString text)
{
	text = text.trimmed();
	text.replace('O', '0').replace('o', '0').replace('D', '0').replace('Q', '0');
	text.replace('I', '1').replace('l', '1').replace('|', '1').replace('!', '1');
	text.replace('S', '5').replace('s', '5');
	text.replace('B', '8');
	text.replace(';', ':').replace('.', ':').replace(',', ':');
	QString out;
	bool lastColon = false;
	for (const QChar ch : text) {
		if (ch.isDigit()) {
			out.append(ch);
			lastColon = false;
		} else if (ch == ':') {
			if (!lastColon)
				out.append(ch);
			lastColon = true;
		}
	}
	while (out.startsWith(':'))
		out.remove(0, 1);
	while (out.endsWith(':'))
		out.chop(1);
	return out;
}

std::optional<int> parseTimeToSeconds(const QString &value)
{
	const QString text = normalizeTimeText(value);
	if (text.isEmpty())
		return std::nullopt;
	const QStringList parts = text.split(':');
	bool ok = false;
	if (parts.size() == 2) {
		const int mm = parts[0].toInt(&ok);
		if (!ok)
			return std::nullopt;
		const int ss = parts[1].toInt(&ok);
		if (!ok || ss >= 60)
			return std::nullopt;
		return mm * 60 + ss;
	}
	if (parts.size() == 3) {
		const int hh = parts[0].toInt(&ok);
		if (!ok)
			return std::nullopt;
		const int mm = parts[1].toInt(&ok);
		if (!ok)
			return std::nullopt;
		const int ss = parts[2].toInt(&ok);
		if (!ok || mm >= 60 || ss >= 60)
			return std::nullopt;
		return hh * 3600 + mm * 60 + ss;
	}
	return std::nullopt;
}

std::vector<Interval> parseIntervals(const QString &text, QString *error = nullptr)
{
	std::vector<Interval> out;
	const QStringList lines = text.split('\n');
	for (int i = 0; i < lines.size(); ++i) {
		const QString original = lines[i];
		const QString line = original.trimmed();
		if (line.isEmpty() || line.startsWith('#'))
			continue;

		QRegularExpression re(R"(^(.+?)\s*(?:-|—|–|>)\s*(.+?)$)");
		const auto m = re.match(line);
		if (!m.hasMatch()) {
			if (error)
				*error =
					QString("Строка %1: нужен формат 00:12-00:28 или 00:12:15-00:12:28").arg(i + 1);
			return {};
		}
		const auto start = parseTimeToSeconds(m.captured(1));
		const auto end = parseTimeToSeconds(m.captured(2));
		if (!start || !end) {
			if (error)
				*error = QString("Строка %1: не понял время").arg(i + 1);
			return {};
		}
		if (*end < *start) {
			if (error)
				*error = QString("Строка %1: конец раньше начала").arg(i + 1);
			return {};
		}
		out.push_back(Interval{*start, *end, line});
	}
	return out;
}

obs_source_t *sourceByName(const QString &name)
{
	if (name.trimmed().isEmpty())
		return nullptr;
	return obs_get_source_by_name(name.toUtf8().constData());
}

bool setSourceFilterEnabled(const QString &sourceName, const QString &filterName, bool enabled,
			    QString *error = nullptr)
{
	obs_source_t *source = sourceByName(sourceName);
	if (!source) {
		if (error)
			*error = "Источник не найден: " + sourceName;
		return false;
	}
	obs_source_t *filter = obs_source_get_filter_by_name(source, filterName.toUtf8().constData());
	if (!filter) {
		if (error)
			*error = "Фильтр не найден: " + filterName;
		obs_source_release(source);
		return false;
	}
	obs_source_set_enabled(filter, enabled);
	obs_source_release(filter);
	obs_source_release(source);
	return true;
}

QStringList enumSources()
{
	QStringList names;
	obs_enum_sources(
		[](void *data, obs_source_t *source) {
			auto *list = static_cast<QStringList *>(data);
			const char *name = obs_source_get_name(source);
			if (name && *name)
				list->append(QString::fromUtf8(name));
			return true;
		},
		&names);
	names.removeDuplicates();
	names.sort(Qt::CaseInsensitive);
	return names;
}

QStringList enumFilters(const QString &sourceName)
{
	QStringList names;
	obs_source_t *source = sourceByName(sourceName);
	if (!source)
		return names;

	obs_source_enum_filters(
		source,
		[](obs_source_t *parent, obs_source_t *filter, void *data) {
			Q_UNUSED(parent);
			auto *list = static_cast<QStringList *>(data);
			const char *name = obs_source_get_name(filter);
			if (name && *name)
				list->append(QString::fromUtf8(name));
		},
		&names);

	obs_source_release(source);
	names.removeDuplicates();
	names.sort(Qt::CaseInsensitive);
	return names;
}

class BridgeServer final : public QTcpServer {
	Q_OBJECT
public:
	explicit BridgeServer(QObject *parent = nullptr) : QTcpServer(parent) {}

signals:
	void snapshotReceived(const Snapshot &snapshot);

protected:
	void incomingConnection(qintptr socketDescriptor) override
	{
		auto *socket = new QTcpSocket(this);
		socket->setSocketDescriptor(socketDescriptor);
		connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { handleReadyRead(socket); });
		connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
	}

private:
	void writeResponse(QTcpSocket *socket, const QByteArray &body, const char *contentType = "application/json",
			   int code = 200, const char *text = "OK")
	{
		QByteArray header;
		header += "HTTP/1.1 " + QByteArray::number(code) + " " + text + "\r\n";
		header += "Content-Type: ";
		header += contentType;
		header += "; charset=utf-8\r\n";
		header += "Access-Control-Allow-Origin: *\r\n";
		header += "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
		header += "Access-Control-Allow-Headers: Content-Type\r\n";
		header += "Connection: close\r\n";
		header += "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
		socket->write(header);
		socket->write(body);
		socket->disconnectFromHost();
	}

	void handleReadyRead(QTcpSocket *socket)
	{
		const QByteArray request = socket->readAll();
		const int headerEnd = request.indexOf("\r\n\r\n");
		if (headerEnd < 0)
			return;
		const QByteArray header = request.left(headerEnd);
		const QByteArray body = request.mid(headerEnd + 4);
		const QList<QByteArray> lines = header.split('\n');
		if (lines.isEmpty()) {
			writeResponse(socket, R"({"ok":false})", "application/json", 400, "Bad Request");
			return;
		}
		const QByteArray requestLine = lines.first().trimmed();
		const QList<QByteArray> parts = requestLine.split(' ');
		const QByteArray method = parts.value(0);
		const QByteArray path = parts.value(1);

		if (method == "OPTIONS") {
			writeResponse(socket, "{}", "application/json");
			return;
		}
		if (method == "GET" && (path == "/status" || path == "/")) {
			writeResponse(
				socket,
				R"({"ok":true,"plugin":"AbsoJlut AutoBlur Raspberry","mode":"dll","has_time":true})");
			return;
		}
		if (method != "POST" || path != "/push") {
			writeResponse(socket, R"({"ok":false,"error":"not_found"})", "application/json", 404,
				      "Not Found");
			return;
		}

		const auto doc = QJsonDocument::fromJson(body);
		if (!doc.isObject()) {
			writeResponse(socket, R"({"ok":false,"error":"bad_json"})", "application/json", 400,
				      "Bad Request");
			return;
		}
		const QJsonObject obj = doc.object();
		const double current = obj.value("currentTime").toDouble(obj.value("current_time").toDouble(-1));
		if (!std::isfinite(current) || current < 0) {
			writeResponse(socket, R"({"ok":false,"error":"currentTime required"})", "application/json", 422,
				      "Unprocessable Entity");
			return;
		}
		Snapshot snap;
		snap.currentTime = current;
		snap.duration = obj.value("duration").toDouble(-1);
		snap.paused = obj.value("paused").toBool(false);
		snap.playbackRate = obj.value("playbackRate").toDouble(obj.value("playback_rate").toDouble(1.0));
		if (!std::isfinite(snap.playbackRate) || snap.playbackRate <= 0)
			snap.playbackRate = 1.0;
		snap.title = obj.value("title").toString();
		snap.url = obj.value("url").toString();
		snap.receivedMs = QDateTime::currentMSecsSinceEpoch();
		emit snapshotReceived(snap);
		writeResponse(socket, R"({"ok":true})");
	}
};

class MovieBlurDock final : public QWidget {
	Q_OBJECT
public:
	explicit MovieBlurDock(QWidget *parent = nullptr) : QWidget(parent), settings_("MovieBlurSync", "OBSPlugin")
	{
		setObjectName("AbsoJlutAutoBlurRaspberryDock");
		setWindowTitle("AbsoJlut AutoBlur Raspberry");
		buildUi();
		loadSettings();
		connectSignals();

		timer_.setInterval(500);
		connect(&timer_, &QTimer::timeout, this, &MovieBlurDock::tick);
		statusTimer_.setInterval(300);
		connect(&statusTimer_, &QTimer::timeout, this, &MovieBlurDock::refreshStatus);
		statusTimer_.start();

		startBridge();
		refreshSources();
	}

	~MovieBlurDock() override
	{
		stopSync();
		server_.close();
	}

private:
	QSettings settings_;
	BridgeServer server_;
	QTimer timer_;
	QTimer statusTimer_;
	std::optional<Snapshot> snapshot_;
	bool running_ = false;
	bool paused_ = false;
	bool blurEnabled_ = false;
	QString lastError_;

	QLabel *timeLabel_ = nullptr;
	QLabel *bridgeLabel_ = nullptr;
	QLabel *blurLabel_ = nullptr;
	QLabel *serverLabel_ = nullptr;
	QComboBox *sourceCombo_ = nullptr;
	QComboBox *filterCombo_ = nullptr;
	QDoubleSpinBox *preRoll_ = nullptr;
	QDoubleSpinBox *postRoll_ = nullptr;
	QDoubleSpinBox *scanInterval_ = nullptr;
	QDoubleSpinBox *maxHidden_ = nullptr;
	QCheckBox *safeMode_ = nullptr;
	QPlainTextEdit *intervalsEdit_ = nullptr;
	QPushButton *startBtn_ = nullptr;
	QPushButton *pauseBtn_ = nullptr;

	void buildUi()
	{
		auto *root = this;
		auto *layout = new QVBoxLayout(root);

		auto *status = new QGroupBox("Статус", root);
		auto *statusLayout = new QVBoxLayout(status);
		timeLabel_ = new QLabel("--:--:--", status);
		QFont big = timeLabel_->font();
		big.setPointSize(20);
		big.setBold(true);
		timeLabel_->setFont(big);
		bridgeLabel_ = new QLabel("Bridge: ждёт данные от сайта", status);
		bridgeLabel_->setWordWrap(true);
		blurLabel_ = new QLabel("Блюр: выключен", status);
		serverLabel_ = new QLabel("Bridge server: —", status);
		statusLayout->addWidget(timeLabel_);
		statusLayout->addWidget(bridgeLabel_);
		statusLayout->addWidget(blurLabel_);
		statusLayout->addWidget(serverLabel_);
		layout->addWidget(status);

		auto *filterBox = new QGroupBox("Источник и фильтр", root);
		auto *form = new QFormLayout(filterBox);
		sourceCombo_ = new QComboBox(filterBox);
		sourceCombo_->setEditable(false);
		filterCombo_ = new QComboBox(filterBox);
		filterCombo_->setEditable(false);
		auto *sourceRow = new QWidget(filterBox);
		auto *sourceRowLayout = new QHBoxLayout(sourceRow);
		sourceRowLayout->setContentsMargins(0, 0, 0, 0);
		auto *refreshSourcesBtn = new QPushButton("Обновить", sourceRow);
		sourceRowLayout->addWidget(sourceCombo_, 1);
		sourceRowLayout->addWidget(refreshSourcesBtn);
		auto *filterRow = new QWidget(filterBox);
		auto *filterRowLayout = new QHBoxLayout(filterRow);
		filterRowLayout->setContentsMargins(0, 0, 0, 0);
		auto *refreshFiltersBtn = new QPushButton("Фильтры", filterRow);
		auto *testBlurBtn = new QPushButton("ON/OFF тест", filterRow);
		filterRowLayout->addWidget(filterCombo_, 1);
		filterRowLayout->addWidget(refreshFiltersBtn);
		filterRowLayout->addWidget(testBlurBtn);
		form->addRow("Источник с блюром", sourceRow);
		form->addRow("Фильтр", filterRow);
		layout->addWidget(filterBox);

		auto *logicBox = new QGroupBox("Логика", root);
		auto *logic = new QFormLayout(logicBox);
		preRoll_ = new QDoubleSpinBox(logicBox);
		preRoll_->setRange(0, 20);
		preRoll_->setDecimals(1);
		preRoll_->setSingleStep(0.1);
		postRoll_ = new QDoubleSpinBox(logicBox);
		postRoll_->setRange(0, 20);
		postRoll_->setDecimals(1);
		postRoll_->setSingleStep(0.1);
		scanInterval_ = new QDoubleSpinBox(logicBox);
		scanInterval_->setRange(0.2, 5);
		scanInterval_->setDecimals(1);
		scanInterval_->setSingleStep(0.1);
		maxHidden_ = new QDoubleSpinBox(logicBox);
		maxHidden_->setRange(0, 120);
		maxHidden_->setDecimals(1);
		maxHidden_->setSingleStep(1.0);
		safeMode_ = new QCheckBox("не снимать блюр при краткой потере bridge", logicBox);
		logic->addRow("Включить раньше, сек", preRoll_);
		logic->addRow("Выключить позже, сек", postRoll_);
		logic->addRow("Проверять каждые, сек", scanInterval_);
		logic->addRow("Считать без данных, сек", maxHidden_);
		logic->addRow("Безопасный режим", safeMode_);
		layout->addWidget(logicBox);

		auto *intervalBox = new QGroupBox("Интервалы блюра", root);
		auto *intervalLayout = new QVBoxLayout(intervalBox);
		intervalsEdit_ = new QPlainTextEdit(intervalBox);
		intervalsEdit_->setPlaceholderText("00:12-00:28\n00:38:44-00:39:10\n01:04:10-01:05:00");
		intervalLayout->addWidget(intervalsEdit_);
		layout->addWidget(intervalBox, 1);

		auto *buttons = new QHBoxLayout();
		startBtn_ = new QPushButton("Старт", root);
		pauseBtn_ = new QPushButton("Пауза", root);
		auto *stopBtn = new QPushButton("Сброс", root);
		buttons->addWidget(startBtn_);
		buttons->addWidget(pauseBtn_);
		buttons->addWidget(stopBtn);
		layout->addLayout(buttons);
		connect(refreshSourcesBtn, &QPushButton::clicked, this, &MovieBlurDock::refreshSources);
		connect(refreshFiltersBtn, &QPushButton::clicked, this, &MovieBlurDock::refreshFilters);
		connect(testBlurBtn, &QPushButton::clicked, this, &MovieBlurDock::toggleBlurTest);
		connect(startBtn_, &QPushButton::clicked, this, &MovieBlurDock::startSync);
		connect(pauseBtn_, &QPushButton::clicked, this, &MovieBlurDock::togglePause);
		connect(stopBtn, &QPushButton::clicked, this, &MovieBlurDock::stopSync);
	}

	void connectSignals()
	{
		connect(&server_, &BridgeServer::snapshotReceived, this, [this](const Snapshot &snapshot) {
			snapshot_ = snapshot;
			refreshStatus();
		});
		connect(sourceCombo_, &QComboBox::currentTextChanged, this, [this]() {
			refreshFilters();
			saveSettings();
		});
		connect(filterCombo_, &QComboBox::currentTextChanged, this, &MovieBlurDock::saveSettings);
		connect(preRoll_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &MovieBlurDock::saveSettings);
		connect(postRoll_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
			&MovieBlurDock::saveSettings);
		connect(scanInterval_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
			&MovieBlurDock::saveSettings);
		connect(maxHidden_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
			&MovieBlurDock::saveSettings);
		connect(safeMode_, &QCheckBox::toggled, this, &MovieBlurDock::saveSettings);
		connect(intervalsEdit_, &QPlainTextEdit::textChanged, this, &MovieBlurDock::saveSettings);
	}

	void loadSettings()
	{
		preRoll_->setValue(settings_.value("pre_roll", 0.5).toDouble());
		postRoll_->setValue(settings_.value("post_roll", 1.0).toDouble());
		scanInterval_->setValue(settings_.value("scan_interval", 0.5).toDouble());
		maxHidden_->setValue(settings_.value("max_hidden_seconds", 20.0).toDouble());
		safeMode_->setChecked(settings_.value("safe_mode", true).toBool());
		intervalsEdit_->setPlainText(
			settings_.value("intervals", "00:12:15-00:12:28\n00:38:44-00:39:10\n01:04:10-01:05:00\n")
				.toString());
	}

private slots:
	void saveSettings()
	{
		settings_.setValue("blur_source", sourceCombo_->currentText());
		settings_.setValue("blur_filter", filterCombo_->currentText());
		settings_.setValue("pre_roll", preRoll_->value());
		settings_.setValue("post_roll", postRoll_->value());
		settings_.setValue("scan_interval", scanInterval_->value());
		settings_.setValue("max_hidden_seconds", maxHidden_->value());
		settings_.setValue("safe_mode", safeMode_->isChecked());
		settings_.setValue("intervals", intervalsEdit_->toPlainText());
	}

	void startBridge()
	{
		if (server_.isListening())
			return;
		if (!server_.listen(QHostAddress::LocalHost, kBridgePort)) {
			serverLabel_->setText("Bridge server: ошибка запуска порта 8799 — " + server_.errorString());
		} else {
			serverLabel_->setText("Bridge server: http://127.0.0.1:8799/push");
		}
	}

	void refreshSources()
	{
		const QString prev = settings_.value("blur_source", sourceCombo_->currentText()).toString();
		sourceCombo_->blockSignals(true);
		sourceCombo_->clear();
		sourceCombo_->addItems(enumSources());
		const int idx = sourceCombo_->findText(prev);
		if (idx >= 0)
			sourceCombo_->setCurrentIndex(idx);
		sourceCombo_->blockSignals(false);
		refreshFilters();
		saveSettings();
	}

	void refreshFilters()
	{
		const QString prev = settings_
					     .value("blur_filter", filterCombo_->currentText().isEmpty()
									   ? "Blur"
									   : filterCombo_->currentText())
					     .toString();
		filterCombo_->blockSignals(true);
		filterCombo_->clear();
		filterCombo_->addItems(enumFilters(sourceCombo_->currentText()));
		const int idx = filterCombo_->findText(prev);
		if (idx >= 0)
			filterCombo_->setCurrentIndex(idx);
		filterCombo_->blockSignals(false);
		saveSettings();
	}

	void toggleBlurTest()
	{
		QString error;
		blurEnabled_ = !blurEnabled_;
		if (!setSourceFilterEnabled(sourceCombo_->currentText(), filterCombo_->currentText(), blurEnabled_,
					    &error)) {
			lastError_ = error;
			blurEnabled_ = !blurEnabled_;
		}
		refreshStatus();
	}

	void startSync()
	{
		QString error;
		const auto intervals = parseIntervals(intervalsEdit_->toPlainText(), &error);
		if (!error.isEmpty()) {
			lastError_ = error;
			refreshStatus();
			return;
		}
		if (intervals.empty()) {
			lastError_ = "Добавь хотя бы один интервал блюра.";
			refreshStatus();
			return;
		}
		running_ = true;
		paused_ = false;
		timer_.setInterval(static_cast<int>(std::max(0.2, scanInterval_->value()) * 1000.0));
		timer_.start();
		lastError_.clear();
		refreshStatus();
	}

	void stopSync()
	{
		running_ = false;
		paused_ = false;
		timer_.stop();
		if (blurEnabled_) {
			QString error;
			setSourceFilterEnabled(sourceCombo_->currentText(), filterCombo_->currentText(), false, &error);
		}
		blurEnabled_ = false;
		refreshStatus();
	}

	void togglePause()
	{
		if (!running_)
			return;
		paused_ = !paused_;
		refreshStatus();
	}

	std::optional<double> currentMovieTime(QString *bridgeText = nullptr) const
	{
		if (!snapshot_) {
			if (bridgeText)
				*bridgeText = "Bridge: нет данных от сайта";
			return std::nullopt;
		}
		const qint64 now = QDateTime::currentMSecsSinceEpoch();
		const double age = std::max(0.0, (now - snapshot_->receivedMs) / 1000.0);
		if (age > maxHidden_->value()) {
			if (bridgeText)
				*bridgeText = QString("Bridge: данные устарели %1 сек").arg(age, 0, 'f', 1);
			return std::nullopt;
		}
		double current = snapshot_->currentTime;
		QString state = "пауза";
		if (!snapshot_->paused) {
			current += age * snapshot_->playbackRate;
			state = QString("x%1").arg(snapshot_->playbackRate, 0, 'g', 3);
		}
		QString title = snapshot_->title.trimmed();
		if (title.isEmpty())
			title = snapshot_->url;
		if (title.size() > 44)
			title = title.left(43) + "…";
		if (bridgeText)
			*bridgeText = QString("Bridge: %1 / %2 / %3")
					      .arg(formatSeconds(current), state, title.isEmpty() ? "video" : title);
		return current;
	}

	void tick()
	{
		if (!running_ || paused_)
			return;
		QString error;
		const auto intervals = parseIntervals(intervalsEdit_->toPlainText(), &error);
		if (!error.isEmpty()) {
			lastError_ = error;
			return;
		}
		QString bridgeText;
		const auto current = currentMovieTime(&bridgeText);
		bool desired = false;
		if (current) {
			desired = std::any_of(intervals.begin(), intervals.end(), [&](const Interval &i) {
				return i.contains(*current, preRoll_->value(), postRoll_->value());
			});
		} else {
			desired = safeMode_->isChecked() ? blurEnabled_ : false;
		}
		if (desired != blurEnabled_) {
			if (setSourceFilterEnabled(sourceCombo_->currentText(), filterCombo_->currentText(), desired,
						   &error)) {
				blurEnabled_ = desired;
			} else {
				lastError_ = error;
			}
		}
		refreshStatus();
	}

	void refreshStatus()
	{
		QString bridgeText;
		const auto current = currentMovieTime(&bridgeText);
		timeLabel_->setText(formatSeconds(current.value_or(-1)));
		bridgeLabel_->setText(bridgeText);
		QString blur = blurEnabled_ ? "Блюр: включен" : "Блюр: выключен";
		if (running_)
			blur.prepend(paused_ ? "⏸ " : "🟢 ");
		if (!lastError_.isEmpty())
			blur += " | Ошибка: " + lastError_;
		blurLabel_->setText(blur);
		pauseBtn_->setText(paused_ ? "Продолжить" : "Пауза");
		startBtn_->setEnabled(!running_);
	}
};

MovieBlurDock *g_dock = nullptr;
bool g_dock_added = false;

} // namespace

bool obs_module_load(void)
{
	blog(LOG_INFO, "[AbsoJlut AutoBlur Raspberry] loading DLL plugin");
	if (!QApplication::instance()) {
		blog(LOG_WARNING, "[AbsoJlut AutoBlur Raspberry] QApplication is not available");
		return true;
	}
	g_dock = new MovieBlurDock();
	g_dock_added =
		obs_frontend_add_dock_by_id("absojlut-autoblur-raspberry", "AbsoJlut AutoBlur Raspberry", g_dock);
	blog(LOG_INFO, "[AbsoJlut AutoBlur Raspberry] dock added");
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[AbsoJlut AutoBlur Raspberry] unloading DLL plugin");
	if (g_dock_added) {
		obs_frontend_remove_dock("absojlut-autoblur-raspberry");
		g_dock_added = false;
		g_dock = nullptr;
	} else if (g_dock) {
		g_dock->deleteLater();
		g_dock = nullptr;
	}
}

#include "plugin-main.moc"
