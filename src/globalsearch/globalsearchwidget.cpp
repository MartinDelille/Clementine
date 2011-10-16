/* This file is part of Clementine.
   Copyright 2010, David Sansome <me@davidsansome.com>

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "config.h"
#include "globalsearch.h"
#include "globalsearchitemdelegate.h"
#include "globalsearchsortmodel.h"
#include "globalsearchtooltip.h"
#include "globalsearchwidget.h"
#include "ui_globalsearchwidget.h"
#include "core/logging.h"
#include "core/stylesheetloader.h"
#include "core/utilities.h"
#include "playlist/playlistview.h"
#include "playlist/songmimedata.h"
#include "ui/qt_blurimage.h"
#include "widgets/stylehelper.h"

#include <QDesktopWidget>
#include <QListView>
#include <QPainter>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>


const int GlobalSearchWidget::kMinVisibleItems = 3;
const int GlobalSearchWidget::kMaxVisibleItems = 25;
const int GlobalSearchWidget::kSwapModelsTimeoutMsec = 250;


GlobalSearchWidget::GlobalSearchWidget(QWidget* parent)
  : QWidget(parent),
    ui_(new Ui_GlobalSearchWidget),
    engine_(NULL),
    last_id_(0),
    order_arrived_counter_(0),
    front_model_(new QStandardItemModel(this)),
    back_model_(new QStandardItemModel(this)),
    current_model_(front_model_),
    front_proxy_(new GlobalSearchSortModel(this)),
    back_proxy_(new GlobalSearchSortModel(this)),
    current_proxy_(front_proxy_),
    view_(new QListView),
    consume_focus_out_(false),
    swap_models_timer_(new QTimer(this)),
    background_(":allthethings.png"),
    desktop_(qApp->desktop()),
    combine_identical_results_(true)
{
  ui_->setupUi(this);
  ReloadSettings();

  // Set up the sorting proxy model
  front_proxy_->setSourceModel(front_model_);
  front_proxy_->setDynamicSortFilter(true);
  front_proxy_->sort(0);

  back_proxy_->setSourceModel(back_model_);
  back_proxy_->setDynamicSortFilter(true);
  back_proxy_->sort(0);

  // Set up the popup
  view_->setObjectName("popup");
  view_->setWindowFlags(Qt::Popup);
  view_->setFocusPolicy(Qt::NoFocus);
  view_->setFocusProxy(ui_->search);
  view_->installEventFilter(this);

  view_->setModel(front_proxy_);
  view_->setItemDelegate(new GlobalSearchItemDelegate(this));
  view_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  view_->setEditTriggers(QAbstractItemView::NoEditTriggers);

  ui_->search->installEventFilter(this);

  // Actions
  add_              = new QAction(tr("Add to playlist"), this);
  add_and_play_     = new QAction(tr("Add and play now"), this);
  add_and_queue_    = new QAction(tr("Queue track"), this);
  replace_          = new QAction(tr("Replace current playlist"), this);
  replace_and_play_ = new QAction(tr("Replace and play now"), this);

  add_->setShortcut(QKeySequence(Qt::Key_Return));
  add_and_play_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
  add_and_queue_->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_Return));
  replace_->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Return));
  replace_and_play_->setShortcut(QKeySequence(Qt::ALT | Qt::CTRL | Qt::Key_Return));

  connect(add_,              SIGNAL(triggered()), SLOT(AddCurrent()));
  connect(add_and_play_,     SIGNAL(triggered()), SLOT(AddAndPlayCurrent()));
  connect(add_and_queue_,    SIGNAL(triggered()), SLOT(AddAndQueueCurrent()));
  connect(replace_,          SIGNAL(triggered()), SLOT(ReplaceCurrent()));
  connect(replace_and_play_, SIGNAL(triggered()), SLOT(ReplaceAndPlayCurrent()));

  actions_ << add_ << add_and_play_ << add_and_queue_ << replace_
           << replace_and_play_;

  // Load style sheets
  StyleSheetLoader* style_loader = new StyleSheetLoader(this);
  style_loader->SetStyleSheet(this, ":globalsearch.css");

  swap_models_timer_->setSingleShot(true);
  swap_models_timer_->setInterval(kSwapModelsTimeoutMsec);

  connect(ui_->search, SIGNAL(textEdited(QString)), SLOT(TextEdited(QString)));
  connect(view_, SIGNAL(doubleClicked(QModelIndex)), SLOT(ResultDoubleClicked()));
  connect(view_->selectionModel(), SIGNAL(currentChanged(QModelIndex,QModelIndex)),
          SLOT(UpdateTooltip()));
  connect(swap_models_timer_, SIGNAL(timeout()), SLOT(SwapModels()));
}

GlobalSearchWidget::~GlobalSearchWidget() {
  delete ui_;
}

void GlobalSearchWidget::Init(GlobalSearch* engine) {
  engine_ = engine;

  // These have to be queued connections because they may get emitted before
  // our call to Search() (or whatever) returns and we add the ID to the map.
  connect(engine_, SIGNAL(ResultsAvailable(int,SearchProvider::ResultList)),
          SLOT(AddResults(int,SearchProvider::ResultList)),
          Qt::QueuedConnection);
  connect(engine_, SIGNAL(ArtLoaded(int,QPixmap)), SLOT(ArtLoaded(int,QPixmap)),
          Qt::QueuedConnection);
  connect(engine_, SIGNAL(TracksLoaded(int,MimeData*)), SLOT(TracksLoaded(int,MimeData*)),
          Qt::QueuedConnection);

  connect(engine_, SIGNAL(ProviderAdded(const SearchProvider*)),
          SLOT(ProviderAdded(const SearchProvider*)));
  connect(engine_, SIGNAL(ProviderRemoved(const SearchProvider*)),
          SLOT(ProviderRemoved(const SearchProvider*)));

  // Take all the ProviderAdded signals we missed.
  foreach (const SearchProvider* provider, engine_->providers()) {
    ProviderAdded(provider);
  }

  view_->setStyle(new PlaylistProxyStyle(style()));

  // The style helper's base color doesn't get initialised until after the
  // constructor.
  QPalette view_palette = view_->palette();
  view_palette.setColor(QPalette::Text, Utils::StyleHelper::panelTextColor());
  view_palette.setColor(QPalette::HighlightedText, QColor(60, 60, 60));
  view_palette.setColor(QPalette::Base, Utils::StyleHelper::shadowColor().darker(109));

  QFont view_font = view_->font();
  view_font.setPointSizeF(Utils::StyleHelper::sidebarFontSize());

  view_->setFont(view_font);
  view_->setPalette(view_palette);
}

void GlobalSearchWidget::resizeEvent(QResizeEvent* e) {
  background_scaled_ = background_.scaled(size(), Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation);

  QWidget::resizeEvent(e);
}

void GlobalSearchWidget::paintEvent(QPaintEvent* e) {
  QPainter p(this);

  QRect total_rect = rect().adjusted(0, 0, 1, 0);
  total_rect = style()->visualRect(layoutDirection(), geometry(), total_rect);
  Utils::StyleHelper::verticalGradient(&p, total_rect, total_rect);

  QRect background_rect = background_scaled_.rect();
  background_rect.moveLeft(total_rect.right() - background_rect.width());
  background_rect.moveTop(total_rect.top());

  p.setOpacity(0.5);
  p.drawPixmap(background_rect, background_scaled_);
  p.setOpacity(1.0);

  p.setPen(Utils::StyleHelper::borderColor());
  p.drawLine(total_rect.topRight(), total_rect.bottomRight());

  QColor light = Utils::StyleHelper::sidebarHighlight();
  p.setPen(light);
  p.drawLine(total_rect.bottomLeft(), total_rect.bottomRight());
}

void GlobalSearchWidget::TextEdited(const QString& text) {
  const QString trimmed_text = text.trimmed();

  if (trimmed_text.length() < 3) {
    RepositionPopup();
    return;
  }

  // Add results to the back model, switch models after some delay.
  back_model_->clear();
  current_model_ = back_model_;
  current_proxy_ = back_proxy_;
  order_arrived_counter_ = 0;
  swap_models_timer_->start();

  // Cancel the last search (if any) and start the new one.
  engine_->CancelSearch(last_id_);
  last_id_ = engine_->SearchAsync(trimmed_text);
}

void GlobalSearchWidget::SwapModels() {
  art_requests_.clear();

  qSwap(front_model_, back_model_);
  qSwap(front_proxy_, back_proxy_);

  view_->setModel(front_proxy_);
  connect(view_->selectionModel(), SIGNAL(currentChanged(QModelIndex,QModelIndex)),
          SLOT(UpdateTooltip()));
  RepositionPopup();
}

void GlobalSearchWidget::AddResults(int id, const SearchProvider::ResultList& results) {
  if (id != last_id_)
    return;

  foreach (const SearchProvider::Result& result, results) {
    QStandardItem* item = new QStandardItem;
    item->setData(QVariant::fromValue(result), Role_PrimaryResult);
    item->setData(QVariant::fromValue(SearchProvider::ResultList() << result), Role_AllResults);
    item->setData(order_arrived_counter_, Role_OrderArrived);

    QPixmap pixmap;
    if (engine_->FindCachedPixmap(result, &pixmap)) {
      item->setData(pixmap, Qt::DecorationRole);
    }

    current_model_->appendRow(item);

    if (combine_identical_results_) {
      // Maybe we can combine this result with an identical result from another
      // provider.  We can use the sorted model to narrow the scope of the
      // search a bit - look at the result after the current one, then all the
      // results before.
      QModelIndex my_proxy_index = current_proxy_->mapFromSource(item->index());
      QModelIndexList candidates;
      candidates << my_proxy_index.sibling(my_proxy_index.row() + 1, 0);

      for (int i=my_proxy_index.row()-1 ; i>=0 ; --i) {
        candidates << my_proxy_index.sibling(i, 0);
      }

      foreach (const QModelIndex& index, candidates) {
        if (!index.isValid())
          continue;

        CombineAction action = CanCombineResults(my_proxy_index, index);

        switch (action) {
        case CannotCombine:
          continue;

        case LeftPreferred:
          CombineResults(my_proxy_index, index);
          break;

        case RightPreferred:
          CombineResults(index, my_proxy_index);
          break;
        }

        // We've just invalidated the indexes so we have to stop.
        break;
      }
    }
  }

  order_arrived_counter_ ++;

  RepositionPopup();
}

void GlobalSearchWidget::RepositionPopup() {
  if (front_model_->rowCount() == 0) {
    HidePopup();
    return;
  }

  int h = view_->sizeHintForRow(0) * float(0.5 +
      qBound(kMinVisibleItems, front_model_->rowCount(), kMaxVisibleItems));
  int w = ui_->search->width();

  const QPoint pos = ui_->search->mapToGlobal(ui_->search->rect().bottomLeft());

  // Shrink the popup if it would otherwise go off the screen
  const QRect screen = desktop_->availableGeometry(ui_->search);
  h = qMin(h, screen.bottom() - pos.y());

  view_->setGeometry(QRect(pos, QSize(w, h)));

  if (!view_->isVisible()) {
    view_->show();
    ui_->search->setFocus();
  }
}

bool GlobalSearchWidget::eventFilter(QObject* o, QEvent* e) {
  if (o == ui_->search)
    return EventFilterSearchWidget(o, e);

  if (o == view_)
    return EventFilterPopup(o, e);

  QToolButton* button = qobject_cast<QToolButton*>(o);
  if (button && provider_buttons_.right.count(button))
    return EventFilterProviderButton(button, e);

  return QWidget::eventFilter(o, e);
}

bool GlobalSearchWidget::EventFilterSearchWidget(QObject* o, QEvent* e) {
  switch (e->type()) {
  case QEvent::FocusOut:
    if (consume_focus_out_ && view_->isVisible())
      return true;
    break;

  case QEvent::FocusIn: {
    QFocusEvent* fe = static_cast<QFocusEvent*>(e);
    switch (fe->reason()) {
    case Qt::MouseFocusReason:
    case Qt::TabFocusReason:
    case Qt::BacktabFocusReason:
      if (!ui_->search->text().isEmpty())
        RepositionPopup();
      break;

    default:
      break;
    }

    break;
  }

  default:
    break;
  }

  return QWidget::eventFilter(o, e);
}

bool GlobalSearchWidget::EventFilterPopup(QObject*, QEvent* e) {
  // Most of this is borrowed from QCompleter::eventFilter

  switch (e->type()) {
  case QEvent::KeyPress: {
    QKeyEvent* ke = static_cast<QKeyEvent*>(e);

    QModelIndex cur_index = view_->currentIndex();
    const int key = ke->key();

    // Handle popup navigation keys. These are hardcoded because up/down might make the
    // widget do something else (lineedit cursor moves to home/end on mac, for instance)
    switch (key) {
    case Qt::Key_End:
    case Qt::Key_Home:
      if (ke->modifiers() & Qt::ControlModifier)
        return false;
      break;

    case Qt::Key_Up:
      if (!cur_index.isValid()) {
        view_->setCurrentIndex(front_proxy_->index(front_proxy_->rowCount() - 1, 0));
        return true;
      } else if (cur_index.row() == 0) {
        return true;
      }
      return false;

    case Qt::Key_Down:
      if (!cur_index.isValid()) {
        view_->setCurrentIndex(front_proxy_->index(0, 0));
        return true;
      } else if (cur_index.row() == front_proxy_->rowCount() - 1) {
        return true;
      }
      return false;

    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
      return false;
    }

    // Send the event to the widget. If the widget accepted the event, do nothing
    // If the widget did not accept the event, provide a default implementation
    consume_focus_out_ = false;
    (static_cast<QObject *>(ui_->search))->event(ke);
    consume_focus_out_ = true;

    if (e->isAccepted() || !view_->isVisible()) {
      // widget lost focus, hide the popup
      if (!ui_->search->hasFocus())
        HidePopup();
      if (e->isAccepted())
        return true;
    }

    // default implementation for keys not handled by the widget when popup is open
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
      // Handle the QActions here - they don't activate when the tooltip is showing
      if (ke->modifiers() & Qt::AltModifier && ke->modifiers() & Qt::ControlModifier)
        replace_and_play_->trigger();
      else if (ke->modifiers() & Qt::AltModifier)
        replace_->trigger();
      else if (ke->modifiers() & Qt::ControlModifier)
        add_and_play_->trigger();
      else if (ke->modifiers() & Qt::ShiftModifier)
        add_and_queue_->trigger();
      else
        add_->trigger();
      break;

    case Qt::Key_F4:
      if (ke->modifiers() & Qt::AltModifier)
        HidePopup();
      break;

    case Qt::Key_Backtab:
    case Qt::Key_Escape:
      HidePopup();
      break;

    default:
      break;
    }

    return true;
  }

  case QEvent::MouseButtonPress:
    if (!view_->underMouse()) {
      HidePopup();
      return true;
    }
    return false;

  case QEvent::InputMethod:
  case QEvent::ShortcutOverride:
    QApplication::sendEvent(ui_->search, e);
    break;

  default:
    return false;
  }

  return false;
}

bool GlobalSearchWidget::EventFilterProviderButton(QToolButton* button, QEvent* e) {
  switch (e->type()) {
  case QEvent::Enter:
    QToolTip::showText(button->mapToGlobal(button->rect().bottomLeft()),
                       button->toolTip(), button);
    break;

  case QEvent::Leave:
    QToolTip::hideText();
    break;

  case QEvent::ToolTip:
    // Ignore normal tooltip events.
    return true;

  default:
    break;
  }

  return false;
}

void GlobalSearchWidget::LazyLoadArt(const QModelIndex& proxy_index) {
  if (!proxy_index.isValid() || proxy_index.data(Role_LazyLoadingArt).isValid()) {
    return;
  }
  if (proxy_index.model() != front_proxy_) {
    return;
  }

  const QModelIndex source_index = front_proxy_->mapToSource(proxy_index);
  front_model_->itemFromIndex(source_index)->setData(true, Role_LazyLoadingArt);

  const SearchProvider::Result result =
      source_index.data(Role_PrimaryResult).value<SearchProvider::Result>();

  int id = engine_->LoadArtAsync(result);
  art_requests_[id] = source_index;
}

void GlobalSearchWidget::ArtLoaded(int id, const QPixmap& pixmap) {
  if (!art_requests_.contains(id))
    return;
  QModelIndex index = art_requests_.take(id);

  front_model_->itemFromIndex(index)->setData(pixmap, Qt::DecorationRole);
}

void GlobalSearchWidget::ResultDoubleClicked() {
  LoadTracks(NULL);
}

void GlobalSearchWidget::AddCurrent() {
  LoadTracks(add_);
}

void GlobalSearchWidget::AddAndPlayCurrent() {
  LoadTracks(add_and_play_);
}

void GlobalSearchWidget::AddAndQueueCurrent() {
  LoadTracks(add_and_queue_);
}

void GlobalSearchWidget::ReplaceCurrent() {
  LoadTracks(replace_);
}

void GlobalSearchWidget::ReplaceAndPlayCurrent() {
  LoadTracks(replace_and_play_);
}

void GlobalSearchWidget::LoadTracks(QAction* trigger) {
  QModelIndex index = view_->currentIndex();
  if (!index.isValid())
    index = front_proxy_->index(0, 0);

  if (!index.isValid())
    return;

  int result_index = 0;
  if (tooltip_ && tooltip_->isVisible()) {
    result_index = tooltip_->ActiveResultIndex();
  }

  const SearchProvider::ResultList results =
      index.data(Role_AllResults).value<SearchProvider::ResultList>();

  if (result_index < 0 || result_index >= results.count())
    return;

  int id = engine_->LoadTracksAsync(results[result_index]);
  track_requests_[id] = trigger;
}

void GlobalSearchWidget::TracksLoaded(int id, MimeData* mime_data) {
  if (!track_requests_.contains(id))
    return;

  QAction* trigger = track_requests_.take(id);

  if (!mime_data)
    return;

  if (trigger == NULL) {
    mime_data->from_doubleclick_ = true;
  } else {
    if (trigger == add_and_play_) {
      mime_data->override_user_settings_ = true;
      mime_data->play_now_ = true;
    } else if (trigger == add_and_queue_) {
      mime_data->enqueue_now_ = true;
    } else if (trigger == replace_) {
      mime_data->clear_first_= true;
    } else if (trigger == replace_and_play_) {
      mime_data->clear_first_ = true;
      mime_data->override_user_settings_ = true;
      mime_data->play_now_ = true;
    }
  }

  emit AddToPlaylist(mime_data);
}

void GlobalSearchWidget::ReloadSettings() {
  QSettings s;
  s.beginGroup(GlobalSearch::kSettingsGroup);

  combine_identical_results_ = s.value("combine_identical_results", true).toBool();
  provider_order_ = s.value("provider_order", QStringList() << "library").toStringList();
}

GlobalSearchWidget::CombineAction GlobalSearchWidget::CanCombineResults(
      const QModelIndex& left, const QModelIndex& right) const {
  const SearchProvider::Result r1 = left.data(Role_PrimaryResult)
      .value<SearchProvider::Result>();
  const SearchProvider::Result r2 = right.data(Role_PrimaryResult)
      .value<SearchProvider::Result>();

  if (r1.match_quality_ != r2.match_quality_ || r1.type_ != r2.type_)
    return CannotCombine;

#define StringsDiffer(field) \
  (QString::compare(r1.metadata_.field(), r2.metadata_.field(), Qt::CaseInsensitive) != 0)

  switch (r1.type_) {
  case globalsearch::Type_Track:
    if (StringsDiffer(title))
      return CannotCombine;
    // fallthrough
  case globalsearch::Type_Album:
    if (StringsDiffer(album) || StringsDiffer(artist))
      return CannotCombine;
    break;
  case globalsearch::Type_Stream:
    if (StringsDiffer(url().toString))
      return CannotCombine;
    break;
  }

#undef StringsDiffer

  // They look the same - decide which provider we like best.
  const int p1 = provider_order_.indexOf(r1.provider_->id());
  const int p2 = provider_order_.indexOf(r2.provider_->id());

  return p2 > p1 ? RightPreferred : LeftPreferred;
}

void GlobalSearchWidget::CombineResults(const QModelIndex& superior, const QModelIndex& inferior) {
  QStandardItem* superior_item = current_model_->itemFromIndex(current_proxy_->mapToSource(superior));
  QStandardItem* inferior_item = current_model_->itemFromIndex(current_proxy_->mapToSource(inferior));

  SearchProvider::ResultList superior_results =
      superior_item->data(Role_AllResults).value<SearchProvider::ResultList>();
  SearchProvider::ResultList inferior_results =
      inferior_item->data(Role_AllResults).value<SearchProvider::ResultList>();

  superior_results.append(inferior_results);
  superior_item->setData(QVariant::fromValue(superior_results), Role_AllResults);

  current_model_->invisibleRootItem()->removeRow(inferior_item->row());
}

void GlobalSearchWidget::HidePopup() {
  if (tooltip_)
    tooltip_->hide();
  view_->hide();
}

void GlobalSearchWidget::UpdateTooltip() {
  if (!view_->isVisible()) {
    if (tooltip_)
      tooltip_->hide();
    return;
  }

  const QModelIndex current = view_->selectionModel()->currentIndex();
  if (!current.isValid())
    return;

  const SearchProvider::ResultList results = current.data(Role_AllResults)
      .value<SearchProvider::ResultList>();

  if (!tooltip_) {
    tooltip_.reset(new GlobalSearchTooltip(view_));
    tooltip_->setFont(view_->font());
    tooltip_->setPalette(view_->palette());
    tooltip_->SetActions(actions_);
  }

  const QRect item_rect = view_->visualRect(current);
  const QPoint popup_pos = item_rect.topRight() +
      QPoint(-GlobalSearchTooltip::kArrowWidth,
             item_rect.height() / 2);

  tooltip_->SetResults(results);
  tooltip_->ShowAt(view_->mapToGlobal(popup_pos));
}

void GlobalSearchWidget::ProviderAdded(const SearchProvider* provider) {
  if (provider_buttons_.left.count(provider)) {
    qLog(Error) << "Tried to add the same provider twice:"
                << provider->name() << provider->id();
    return;
  }

  // Create a button for the provider.
  QToolButton* button = new QToolButton(this);
  button->setToolTip(tr("Show results from %1").arg(provider->name()));
  button->setCheckable(true);
  button->setChecked(engine_->is_provider_enabled(provider));
  button->installEventFilter(this);

  // Make the "Off" icon state greyed out, semi transparent and blurred.
  QImage disabled_image = provider->icon().pixmap(
        button->iconSize(), QIcon::Disabled).toImage();

  QImage off_image = QImage(disabled_image.size(), QImage::Format_ARGB32);
  off_image.fill(Qt::transparent);

  QPainter p(&off_image);
  p.setOpacity(0.5);
  qt_blurImage(&p, disabled_image, 3.0, true, false);
  p.end();

  QIcon icon;
  icon.addPixmap(provider->icon().pixmap(button->iconSize(), QIcon::Normal),
                 QIcon::Normal, QIcon::On);
  icon.addPixmap(QPixmap::fromImage(off_image), QIcon::Normal, QIcon::Off);

  button->setIcon(icon);

  connect(button, SIGNAL(toggled(bool)), SLOT(ProviderButtonToggled(bool)));

  // Find the appropriate insertion point.
  bool inserted = false;
  for (int i = 0; i < ui_->provider_layout->count(); ++i) {
    const QToolButton* item_button = static_cast<QToolButton*>(
        ui_->provider_layout->itemAt(i)->widget());

    if (!item_button) {
      continue;
    }
    const QString& name = provider_buttons_.right.find(item_button)->second->name();
    if (provider->name() < name) {
      ui_->provider_layout->insertWidget(i, button);
      inserted = true;
      break;
    }
  }

  if (!inserted) {
    // Insert it at the end but before the spacer.
    ui_->provider_layout->insertWidget(ui_->provider_layout->count() - 1, button);
  }

  provider_buttons_.insert(ProviderButton(provider, button));
}

void GlobalSearchWidget::ProviderRemoved(const SearchProvider* provider) {
  if (!provider_buttons_.left.count(provider)) {
    qLog(Error) << "Tried to remove a provider that hadn't been added yet:"
                << provider->name() << provider->id();
    return;
  }

  ProviderButtonMap::left_const_iterator it = provider_buttons_.left.find(provider);
  delete it->second;
  provider_buttons_.left.erase(provider);
}

void GlobalSearchWidget::ProviderButtonToggled(bool on) {
  QToolButton* button = qobject_cast<QToolButton*>(sender());
  if (!button)
    return;

  const SearchProvider* provider = provider_buttons_.right.find(button)->second;
  if (!provider)
    return;

  engine_->SetProviderEnabled(provider, on);
}
