#include "pch.h"
#include "stats-integration.h"

#include <algorithm>

#include <QApplication>
#include <QGridLayout>
#include <QPointer>

#include <util/platform.h>

namespace {

const char* const kRowsObjectName = "obs-multi-rtmp-stats-rows";
const char* const kNoGridProperty = "obs-multi-rtmp-stats-unsupported";

// OBSBasicStats' output table: Output, Status, Dropped Frames, Total Data
// Output, Bitrate.
constexpr int kOutputColumnCount = 5;

// Interval of OBSBasicStats' own refresh timer (TIMER_INTERVAL there).
constexpr int kStatsRefreshInterval = 2000;

QString FrontendText(const char* key)
{
    return QString::fromUtf8(obs_frontend_get_locale_string(key));
}

// Same as setClasses() in OBS's Qt wrappers, which plugins can't link against.
// OBS themes color the stats labels through the "class" property.
void SetClasses(QWidget* widget, const QString& classes)
{
    if (widget->property("class").toString() == classes)
        return;

    widget->setProperty("class", classes);

    // force style sheet recalculation
    QString qss = widget->styleSheet();
    widget->setStyleSheet("/* */");
    widget->setStyleSheet(qss);
}

// The output table is the grid whose top-left header reads "Output". Matching
// on that header instead of the widget nesting keeps this working if OBS
// rearranges the containers around it.
QGridLayout* FindOutputGrid(QWidget* stats)
{
    const QString outputHeader = FrontendText("Basic.Settings.Output");
    for (auto grid : stats->findChildren<QGridLayout*>()) {
        if (grid->columnCount() != kOutputColumnCount || grid->rowCount() < 1)
            continue;

        auto item = grid->itemAtPosition(0, 0);
        auto label = item ? qobject_cast<QLabel*>(item->widget()) : nullptr;
        if (label && label->text() == outputHeader)
            return grid;
    }
    return nullptr;
}

// The rows added to one Stats window or dock. Lives as a child of that
// window so it goes away with it.
class StatsRows final : public QObject
{
    struct Row {
        std::string id;
        QPointer<QLabel> name;
        QPointer<QLabel> status;
        QPointer<QLabel> droppedFrames;
        QPointer<QLabel> megabytesSent;
        QPointer<QLabel> bitrate;

        // Only compared, never dereferenced: targets create a new output on
        // every start, and the counters below belong to one output.
        obs_output_t* lastOutput = nullptr;
        uint64_t lastBytesSent = 0;
        uint64_t lastBytesSentTime = 0;
        int firstTotal = 0;
        int firstDropped = 0;
    };

    QPointer<QWidget> stats_;
    QPointer<QGridLayout> grid_;
    StatsTargetProvider provider_;
    int firstRow_ = 0;
    std::vector<Row> rows_;

public:
    StatsRows(QWidget* stats, QGridLayout* grid, StatsTargetProvider provider)
        : QObject(stats)
        , stats_(stats)
        , grid_(grid)
        , provider_(std::move(provider))
        , firstRow_(grid->rowCount())
    {
        setObjectName(kRowsObjectName);

        // Refresh right after OBS refreshes its own rows. Its timer only
        // runs while the window is visible.
        QTimer* refreshTimer = nullptr;
        for (auto timer : stats->findChildren<QTimer*>(Qt::FindDirectChildrenOnly)) {
            if (timer->interval() == kStatsRefreshInterval) {
                refreshTimer = timer;
                break;
            }
        }
        if (!refreshTimer) {
            refreshTimer = new QTimer(this);
            refreshTimer->setInterval(kStatsRefreshInterval);
            refreshTimer->start();
        }
        QObject::connect(refreshTimer, &QTimer::timeout, this, [this]() {
            if (stats_ && stats_->isVisible())
                Update();
        });

        const QString resetText = FrontendText("Reset");
        for (auto button : stats->findChildren<QPushButton*>()) {
            if (button->text() == resetText)
                QObject::connect(button, &QPushButton::clicked, this, [this]() { Reset(); });
        }

        Update();
    }

    ~StatsRows()
    {
        ClearRows();
    }

    void Update()
    {
        if (!grid_)
            return;

        auto targets = provider_();
        SyncRows(targets);
        for (size_t i = 0; i < targets.size(); ++i)
            UpdateRow(rows_[i], targets[i].output);
    }

    // Mirrors the Stats window's Reset button for the dropped frames column.
    void Reset()
    {
        if (!grid_)
            return;

        auto targets = provider_();
        SyncRows(targets);
        for (size_t i = 0; i < targets.size(); ++i) {
            obs_output_t* output = targets[i].output;
            if (!output)
                continue;
            rows_[i].lastOutput = output;
            rows_[i].firstTotal = obs_output_get_total_frames(output);
            rows_[i].firstDropped = obs_output_get_frames_dropped(output);
        }
        Update();
    }

private:
    static QString DisplayName(const StatsTargetInfo& target)
    {
        if (target.name.empty())
            return QString::fromUtf8(obs_module_text("NewStreaming"));
        return QString::fromUtf8(target.name);
    }

    void SyncRows(const std::vector<StatsTargetInfo>& targets)
    {
        bool sameTargets = targets.size() == rows_.size();
        for (size_t i = 0; sameTargets && i < targets.size(); ++i)
            sameTargets = targets[i].id == rows_[i].id;

        if (!sameTargets) {
            ClearRows();
            for (auto& target : targets)
                AddRow(target.id);
        }

        for (size_t i = 0; i < targets.size(); ++i) {
            auto name = DisplayName(targets[i]);
            if (rows_[i].name && rows_[i].name->text() != name)
                rows_[i].name->setText(name);
        }
    }

    void AddRow(const std::string& id)
    {
        auto parent = grid_->parentWidget();
        int row = firstRow_ + static_cast<int>(rows_.size());

        Row r;
        r.id = id;
        r.name = new QLabel(parent);
        r.name->setToolTip(QString::fromUtf8(obs_module_text("Title")));
        r.status = new QLabel(parent);
        r.droppedFrames = new QLabel(parent);
        r.megabytesSent = new QLabel(parent);
        r.bitrate = new QLabel(parent);

        int col = 0;
        grid_->addWidget(r.name, row, col++);
        grid_->addWidget(r.status, row, col++);
        grid_->addWidget(r.droppedFrames, row, col++);
        grid_->addWidget(r.megabytesSent, row, col++);
        grid_->addWidget(r.bitrate, row, col++);

        rows_.push_back(std::move(r));
    }

    void ClearRows()
    {
        for (auto& row : rows_) {
            delete row.name;
            delete row.status;
            delete row.droppedFrames;
            delete row.megabytesSent;
            delete row.bitrate;
        }
        rows_.clear();
    }

    // Same calculation and formatting as OBSBasicStats::OutputLabels::Update()
    // for the Stream row.
    static void UpdateRow(Row& row, obs_output_t* output)
    {
        if (!row.name || !row.status || !row.droppedFrames || !row.megabytesSent || !row.bitrate)
            return;

        if (output != row.lastOutput) {
            row.lastOutput = output;
            row.lastBytesSent = 0;
            row.firstTotal = 0;
            row.firstDropped = 0;
        }

        uint64_t totalBytes = output ? obs_output_get_total_bytes(output) : 0;
        uint64_t curTime = os_gettime_ns();
        uint64_t bytesSent = totalBytes;

        if (bytesSent < row.lastBytesSent)
            bytesSent = 0;
        if (bytesSent == 0)
            row.lastBytesSent = 0;

        uint64_t bitsBetween = (bytesSent - row.lastBytesSent) * 8;
        long double timePassed = (long double)(curTime - row.lastBytesSentTime) / 1000000000.0l;
        long double kbps = (long double)bitsBetween / timePassed / 1000.0l;
        if (timePassed < 0.01l)
            kbps = 0.0l;

        QString str = FrontendText("Basic.Stats.Status.Inactive");
        QString styling;
        if (output && obs_output_active(output)) {
            if (obs_output_reconnecting(output)) {
                str = FrontendText("Basic.Stats.Status.Reconnecting");
                styling = "text-danger";
            } else {
                str = FrontendText("Basic.Stats.Status.Live");
                styling = "text-success";
            }
        }
        row.status->setText(str);
        SetClasses(row.status, styling);

        double num = (double)totalBytes / (1024.0 * 1024.0);
        const char* unit = "MiB";
        if (num > 1024) {
            num /= 1024;
            unit = "GiB";
        }
        row.megabytesSent->setText(QString("%1 %2").arg(num, 0, 'f', 1).arg(unit));

        num = (double)kbps;
        unit = "kb/s";
        if (num >= 10'000) {
            num /= 1000;
            unit = "Mb/s";
        }
        row.bitrate->setText(QString("%1 %2").arg(num, 0, 'f', 0).arg(unit));

        int total = output ? obs_output_get_total_frames(output) : 0;
        int dropped = output ? obs_output_get_frames_dropped(output) : 0;

        if (total < row.firstTotal || dropped < row.firstDropped) {
            row.firstTotal = 0;
            row.firstDropped = 0;
        }

        total -= row.firstTotal;
        dropped -= row.firstDropped;

        num = total ? (double)dropped / (double)total * 100.0 : 0.0;

        str = QString("%1 / %2 (%3%)")
            .arg(QString::number(dropped), QString::number(total), QString::number(num, 'f', 1));
        row.droppedFrames->setText(str);

        if (num > 5.0)
            SetClasses(row.droppedFrames, "text-danger");
        else if (num > 1.0)
            SetClasses(row.droppedFrames, "text-warning");
        else
            SetClasses(row.droppedFrames, "");

        row.lastBytesSent = bytesSent;
        row.lastBytesSentTime = curTime;
    }
};

// Both the View > Stats window (created on demand, without a parent) and the
// Stats dock are OBSBasicStats instances, so new ones are picked up as they
// are shown.
class StatsWindowWatcher final : public QObject
{
    StatsTargetProvider provider_;
    std::vector<QPointer<StatsRows>> rows_;

public:
    explicit StatsWindowWatcher(StatsTargetProvider provider)
        : provider_(std::move(provider))
    {
    }

    ~StatsWindowWatcher()
    {
        for (auto& rows : rows_)
            delete rows.data();
    }

    void Attach(QWidget* stats)
    {
        if (stats->findChild<QObject*>(kRowsObjectName, Qt::FindDirectChildrenOnly))
            return;

        auto grid = FindOutputGrid(stats);
        if (!grid) {
            // Shown again on every open; only report it once per window.
            if (!stats->property(kNoGridProperty).toBool()) {
                stats->setProperty(kNoGridProperty, true);
                blog(LOG_WARNING, TAG "Output table of the Stats window not found, targets won't be listed there.");
            }
            return;
        }

        rows_.erase(std::remove_if(rows_.begin(), rows_.end(), [](auto& rows) { return rows.isNull(); }), rows_.end());
        rows_.emplace_back(new StatsRows(stats, grid, provider_));
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Show && watched->isWidgetType() && watched->inherits("OBSBasicStats"))
            Attach(static_cast<QWidget*>(watched));
        return false;
    }
};

StatsWindowWatcher* s_watcher = nullptr;

} // namespace

void InstallStatsIntegration(StatsTargetProvider provider)
{
    if (s_watcher || !qApp)
        return;

    s_watcher = new StatsWindowWatcher(std::move(provider));
    qApp->installEventFilter(s_watcher);

    for (auto widget : QApplication::allWidgets()) {
        if (widget->inherits("OBSBasicStats") && widget->isVisible())
            s_watcher->Attach(widget);
    }
}

void UninstallStatsIntegration()
{
    if (!s_watcher)
        return;

    if (qApp)
        qApp->removeEventFilter(s_watcher);
    delete s_watcher;
    s_watcher = nullptr;
}
