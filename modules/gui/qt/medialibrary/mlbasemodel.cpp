/*****************************************************************************
 * Copyright (C) 2019 VLC authors and VideoLAN
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * ( at your option ) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include <cassert>
#include "medialib.hpp"
#include <vlc_cxx_helpers.hpp>

#include "util/listcache.hpp"

// MediaLibrary includes
#include "mlbasemodel.hpp"
#include "mlhelper.hpp"

#include "util/asynctask.hpp"

using MLListCache = ListCache<std::unique_ptr<MLItem>>;

template<>
bool MLListCache::compareItems(const ItemType& a, const ItemType& b)
{
    return a->getId() == b->getId();
}

static constexpr ssize_t COUNT_UNINITIALIZED = MLListCache::COUNT_UNINITIALIZED;

MLBaseModel::MLBaseModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_ml_event_handle( nullptr, [this](vlc_ml_event_callback_t* cb ) {
            assert( m_mediaLib != nullptr );
            m_mediaLib->unregisterEventListener( cb );
        })
{
    connect( this, &MLBaseModel::resetRequested, this, &MLBaseModel::onResetRequested );

    connect( this, &MLBaseModel::mlChanged, this, &MLBaseModel::loadingChanged );
    connect( this, &MLBaseModel::countChanged, this, &MLBaseModel::loadingChanged );
}

/* For std::unique_ptr, see Effective Modern C++, Item 22 */
MLBaseModel::~MLBaseModel() = default;

void MLBaseModel::sortByColumn(QByteArray name, Qt::SortOrder order)
{
    vlc_ml_sorting_criteria_t sort = nameToCriteria(name);
    bool desc = (order == Qt::SortOrder::DescendingOrder);
    if (m_sort_desc == desc && m_sort == sort)
        return;

    m_sort_desc = (order == Qt::SortOrder::DescendingOrder);
    m_sort = nameToCriteria(name);
    resetCache();
}

//-------------------------------------------------------------------------------------------------

/* Q_INVOKABLE */ QMap<QString, QVariant> MLBaseModel::getDataAt(const QModelIndex & index)
{
    QMap<QString, QVariant> dataDict;

    QHash<int, QByteArray> roles = roleNames();

    for (int role: roles.keys())
    {
        dataDict[roles[role]] = data(index, role);
    }

    return dataDict;
}

/* Q_INVOKABLE */ QMap<QString, QVariant> MLBaseModel::getDataAt(int idx)
{
    return getDataAt(index(idx));
}


quint64 MLBaseModel::loadItems(const QVector<int> &indexes, MLBaseModel::ItemCallback cb)
{
    if (!m_itemLoader)
        m_itemLoader = createLoader();

    return m_itemLoader->loadItemsTask(indexes, cb);
}


void MLBaseModel::getData(const QModelIndexList &indexes, QJSValue callback)
{
    if (!callback.isCallable()) // invalid argument
        return;

    QVector<int> indx;
    std::transform(indexes.begin(), indexes.end()
                   , std::back_inserter(indx)
                   , std::mem_fn(&QModelIndex::row));

    std::shared_ptr<quint64> requestId = std::make_shared<quint64>();

    ItemCallback cb = [this, indxSize = indx.size(), callback, requestId]
    (quint64 id, std::vector<std::unique_ptr<MLItem>> &items) mutable
    {
        auto jsEngine = qjsEngine(this);
         if (!jsEngine || *requestId != id)
            return;

        assert((int)items.size() == indxSize);

        const QHash<int, QByteArray> roles = roleNames();
        auto jsArray = jsEngine->newArray(indxSize);

        for (int i = 0; i < indxSize; ++i)
        {
            const auto &item = items[i];
            QMap<QString, QVariant> dataDict;

            if (item) // item may fail to load
                for (int role: roles.keys())
                    dataDict[roles[role]] = itemRoleData(item.get(), role);

            jsArray.setProperty(i, jsEngine->toScriptValue(dataDict));
        }

        callback.call({jsArray});
    };

    *requestId = loadItems(indx, cb);
}

QVariant MLBaseModel::data(const QModelIndex &index, int role) const
{
    const auto mlItem = item(index.row());
    if (mlItem)
        return itemRoleData(mlItem, role);

    return {};
}

void MLBaseModel::addAndPlay(const QModelIndexList &list, const QStringList &options)
{
    QVector<int> indx;
    std::transform(list.begin(), list.end(), std::back_inserter(indx), std::mem_fn(&QModelIndex::row));

    ItemCallback play = [this, options](quint64, std::vector<std::unique_ptr<MLItem>> &items)
    {
        if (!m_mediaLib)
            return;

        QVariantList ids;
        std::transform(items.begin(), items.end()
                       , std::back_inserter(ids)
                       , [](const std::unique_ptr<MLItem> &item) { return item ? QVariant::fromValue(item->getId()) : QVariant {}; });

        m_mediaLib->addAndPlay(ids, options);
    };

    loadItems(indx, play);
}

//-------------------------------------------------------------------------------------------------

void MLBaseModel::onResetRequested()
{
    invalidateCache();
}

void MLBaseModel::onLocalSizeChanged(size_t queryCount, size_t maximumCount)
{
    emit countChanged(queryCount);
    emit maximumCountChanged(maximumCount);
}

void MLBaseModel::onVlcMlEvent(const MLEvent &event)
{
    switch(event.i_type)
    {
        case VLC_ML_EVENT_BACKGROUND_IDLE_CHANGED:
            if ( event.background_idle_changed.b_idle && m_need_reset )
            {
                emit resetRequested();
                m_need_reset = false;
            }
            break;
        case VLC_ML_EVENT_MEDIA_THUMBNAIL_GENERATED:
        {
            if (event.media_thumbnail_generated.b_success) {
                if (!m_cache)
                    break;

                ssize_t stotal = m_cache->queryCount();
                if (stotal == COUNT_UNINITIALIZED)
                    break;

                int row = 0;

                /* Only consider items available locally in cache */
                MLItemId itemId{event.media_thumbnail_generated.i_media_id, VLC_ML_PARENT_UNKNOWN};
                MLItem* item = findInCache(itemId, &row);
                if (item)
                {
                    vlc_ml_thumbnail_status_t status = VLC_ML_THUMBNAIL_STATUS_FAILURE;
                    QString mrl;
                    if (event.media_thumbnail_generated.b_success)
                    {
                        mrl = qfu(event.media_thumbnail_generated.psz_mrl);
                        status = event.media_thumbnail_generated.i_status;
                    }
                    thumbnailUpdated(index(row), item, mrl, status);
                }
            }
            break;
        }
    }

    if (m_mediaLib && m_mediaLib->idle() && m_need_reset)
    {
        emit resetRequested();
        m_need_reset = false;
    }
}

QString MLBaseModel::getFirstSymbol(QString str)
{
    QString ret("#");
    if ( str.length() > 0 && str[0].isLetter() )
        ret = str[0].toUpper();
    return ret;
}

void MLBaseModel::onVlcMlEvent(void* data, const vlc_ml_event_t* event)
{
    auto self = static_cast<MLBaseModel*>(data);
    //MLEvent is not copiable, but lambda needs to be copiable
    auto  mlEvent = std::make_shared<MLEvent>(event);
    QMetaObject::invokeMethod(self, [self, mlEvent] () mutable {
        self->onVlcMlEvent(*mlEvent);
    });
}

void MLBaseModel::classBegin()
{
    m_qmlInitializing = true;
}

void MLBaseModel::componentComplete()
{
    m_qmlInitializing = false;

    validateCache();
}

MLItemId MLBaseModel::parentId() const
{
    return m_parent;
}

void MLBaseModel::setParentId(MLItemId parentId)
{
    m_parent = parentId;
    resetCache();
    emit parentIdChanged();
}

void MLBaseModel::unsetParentId()
{
    m_parent = MLItemId();
    resetCache();
    emit parentIdChanged();
}

MediaLib* MLBaseModel::ml() const
{
    return m_mediaLib;
}

void MLBaseModel::setMl(MediaLib* medialib)
{
    assert(medialib);

    if (m_mediaLib == medialib)
        return;

    m_mediaLib = medialib;
    if ( m_ml_event_handle == nullptr )
        m_ml_event_handle.reset( m_mediaLib->registerEventListener(onVlcMlEvent, this ) );

    validateCache();

    mlChanged();
}

const QString& MLBaseModel::searchPattern() const
{
    return m_search_pattern;
}

void MLBaseModel::setSearchPattern( const QString& pattern )
{
    QString patternToApply = pattern.length() == 0 ? QString{} : pattern;
    if (patternToApply == m_search_pattern)
        /* No changes */
        return;

    m_search_pattern = patternToApply;
    resetCache();
}

Qt::SortOrder MLBaseModel::getSortOrder() const
{
    return m_sort_desc ? Qt::SortOrder::DescendingOrder : Qt::SortOrder::AscendingOrder;
}

void MLBaseModel::setSortOrder(Qt::SortOrder order)
{
    bool desc = (order == Qt::SortOrder::DescendingOrder);
    if (m_sort_desc == desc)
        return;
    m_sort_desc = desc;
    resetCache();
    emit sortOrderChanged();
}

const QString MLBaseModel::getSortCriteria() const
{
    return criteriaToName(m_sort);
}

void MLBaseModel::setSortCriteria(const QString& criteria)
{
    vlc_ml_sorting_criteria_t sort = nameToCriteria(qtu(criteria));
    if (m_sort == sort)
        return;
    m_sort = sort;
    resetCache();
    emit sortCriteriaChanged();
}

void MLBaseModel::unsetSortCriteria()
{
    if (m_sort == VLC_ML_SORTING_DEFAULT)
        return;

    m_sort = VLC_ML_SORTING_DEFAULT;
    resetCache();
    emit sortCriteriaChanged();
}


unsigned int MLBaseModel::getLimit() const
{
    return m_limit;
}

void MLBaseModel::setLimit(unsigned int limit)
{
    if (m_limit == limit)
        return;
    m_limit = limit;
    resetCache();
    emit limitChanged();
}

unsigned int MLBaseModel::getOffset() const
{
    return m_offset;
}

void MLBaseModel::setOffset(unsigned int offset)
{
    if (m_offset == offset)
        return;
    m_offset = offset;
    resetCache();
    emit offsetChanged();
}


int MLBaseModel::rowCount(const QModelIndex &parent) const
{
    if (!m_cache || parent.isValid())
        return 0;

    return m_cache->queryCount();
}

//-------------------------------------------------------------------------------------------------

unsigned MLBaseModel::getCount() const
{
    if (!m_cache || m_cache->queryCount() == COUNT_UNINITIALIZED)
        return 0;

    return static_cast<unsigned>(m_cache->queryCount());
}

unsigned MLBaseModel::getMaximumCount() const
{
    if (!m_cache || m_cache->maximumCount() == COUNT_UNINITIALIZED)
        return 0;

    return static_cast<unsigned>(m_cache->maximumCount());
}


void MLBaseModel::onCacheDataChanged(int first, int last)
{
    emit dataChanged(index(first), index(last));
}

void MLBaseModel::onCacheBeginInsertRows(int first, int last)
{
    emit beginInsertRows({}, first, last);
}

void MLBaseModel::onCacheBeginRemoveRows(int first, int last)
{
    emit beginRemoveRows({}, first, last);
}

void MLBaseModel::onCacheBeginMoveRows(int first, int last, int destination)
{
    emit beginMoveRows({}, first, last, {}, destination);
}

void MLBaseModel::validateCache() const
{
    if (m_cache)
        return;

    if (!cachable())
        return;

    auto loader = createLoader();
    m_cache = std::make_unique<MLListCache>(std::move(loader), false, m_limit, m_offset);
    connect(m_cache.get(), &MLListCache::localSizeChanged,
            this, &MLBaseModel::onLocalSizeChanged);

    connect(m_cache.get(), &MLListCache::localDataChanged,
            this, &MLBaseModel::onCacheDataChanged);

    connect(m_cache.get(), &MLListCache::beginInsertRows,
            this, &MLBaseModel::onCacheBeginInsertRows);
    connect(m_cache.get(), &MLListCache::endInsertRows,
            this, &MLBaseModel::endInsertRows);

    connect(m_cache.get(), &MLListCache::beginRemoveRows,
            this, &MLBaseModel::onCacheBeginRemoveRows);
    connect(m_cache.get(), &MLListCache::endRemoveRows,
            this, &MLBaseModel::endRemoveRows);

    connect(m_cache.get(), &MLListCache::endMoveRows,
            this, &MLBaseModel::endMoveRows);
    connect(m_cache.get(), &MLListCache::beginMoveRows,
            this, &MLBaseModel::onCacheBeginMoveRows);

    m_cache->initCount();

    emit loadingChanged();
}


void MLBaseModel::resetCache()
{
    beginResetModel();
    m_cache.reset();
    endResetModel();
    validateCache();
}

void MLBaseModel::invalidateCache()
{
    if (m_cache)
    {
        m_cache->invalidate();
        emit loadingChanged();
    }
    else
        validateCache();
}

//-------------------------------------------------------------------------------------------------

MLItem *MLBaseModel::item(int signedidx) const
{
    if (!m_cache)
        return nullptr;

    ssize_t count = m_cache->queryCount();

    if (count == 0 || signedidx < 0 || signedidx >= count)
        return nullptr;

    unsigned int idx = static_cast<unsigned int>(signedidx);

    m_cache->refer(idx);

    const std::unique_ptr<MLItem> *item = m_cache->get(idx);

    if (!item)
        /* Not in cache */
        return nullptr;

    /* Return raw pointer */
    return item->get();
}

MLItem *MLBaseModel::itemCache(int signedidx) const
{
    unsigned int idx = static_cast<unsigned int>(signedidx);

    if (!m_cache)
        return nullptr;

    const std::unique_ptr<MLItem> *item = m_cache->get(idx);

    if (!item)
        /* Not in cache */
        return nullptr;

    /* Return raw pointer */
    return item->get();
}

MLItem *MLBaseModel::findInCache(const MLItemId& id, int *index) const
{
    const auto item = m_cache->find([id](const auto &item)
    {
        return item->getId() == id;
    }, index);

    return item ? item->get() : nullptr;
}

void MLBaseModel::updateItemInCache(const MLItemId& mlid)
{
    if (!m_cache)
    {
        emit resetRequested();
        return;
    }
    MLItem* item = findInCache(mlid, nullptr);
    if (!item) // items isn't loaded
        return;

    if (!m_itemLoader)
        m_itemLoader = createLoader();
    m_itemLoader->loadItemByIdTask(mlid,
        [this](qint64, std::unique_ptr<MLItem>&& item) {
            m_cache->updateItem(std::move(item));
    });
}

void MLBaseModel::deleteItemInCache(const MLItemId& mlid)
{
    if (!m_cache)
    {
        emit resetRequested();
        return;
    }
    m_cache->deleteItem([mlid](const MLListCache::ItemType& item){
        return item->getId() == mlid;
    });
}


void MLBaseModel::moveRangeInCache(int first, int last, int to)
{
    if (!m_cache)
    {
        emit resetRequested();
        return;
    }
    m_cache->moveRange(first, last, to);
}

void MLBaseModel::deleteRangeInCache(int first, int last)
{
    if (!m_cache)
    {
        emit resetRequested();
        return;
    }
    m_cache->deleteRange(first, last);
}


bool MLBaseModel::loading() const

{
    return !(m_mediaLib && m_cache && (m_cache->queryCount() != COUNT_UNINITIALIZED));
}

//-------------------------------------------------------------------------------------------------

MLListCacheLoader::MLListCacheLoader(MediaLib* medialib, std::shared_ptr<MLListCacheLoader::MLOp> op, QObject* parent)
    : QObject(parent)
    , m_medialib(medialib)
    , m_op(op)
{
}

void MLListCacheLoader::cancelTask(size_t taskId)
{
    m_medialib->cancelMLTask(this, taskId);
}

size_t MLListCacheLoader::countTask(std::function<void(size_t taskId, size_t count)> cb)
{
    struct Ctx {
        size_t count;
    };

    return m_medialib->runOnMLThread<Ctx>(this,
        //ML thread
        [op = m_op]
        (vlc_medialibrary_t* ml, Ctx& ctx) {
            auto query = op->getQueryParams();
            ctx.count = op->count(ml, &query);
        },
        //UI thread
        [this, cb](quint64 taskId, Ctx& ctx)
        {
            cb(taskId,  ctx.count);
        });
}

size_t MLListCacheLoader::loadTask(size_t offset, size_t limit,
    std::function<void (size_t, std::vector<ItemType>&)> cb)
{
    struct Ctx {
        std::vector<MLListCacheLoader::ItemType> list;
    };

    return m_medialib->runOnMLThread<Ctx>(this,
        //ML thread
        [op = m_op, offset, limit]
        (vlc_medialibrary_t* ml, Ctx& ctx)
        {
            auto query = op->getQueryParams(offset, limit);
            ctx.list = op->load(ml, &query);
        },
        //UI thread
        [this, cb](quint64 taskId, Ctx& ctx)
        {
            cb(taskId, ctx.list);
        });
}

size_t MLListCacheLoader::countAndLoadTask(size_t offset, size_t limit,
    std::function<void (size_t, size_t, std::vector<ItemType>&)> cb)
{
    struct Ctx {
        size_t maximumCount;
        std::vector<ItemType> list;
    };

    return m_medialib->runOnMLThread<Ctx>(this,
        //ML thread
        [this, offset, limit, op = m_op]
        (vlc_medialibrary_t* ml, Ctx& ctx) {
            auto query = op->getQueryParams(offset, limit);
            ctx.list = op->load(ml, &query);
            ctx.maximumCount = op->count(ml, &query);
        },
        //UI thread
        [this, cb](quint64 taskId, Ctx& ctx) {
            cb(taskId,  ctx.maximumCount, ctx.list);
        });
}

quint64 MLListCacheLoader::loadItemsTask(const QVector<int> &indexes, MLBaseModel::ItemCallback cb)
{
    struct Ctx
    {
        std::vector<std::unique_ptr<MLItem>> items;
    };

    return m_medialib->runOnMLThread<Ctx>(this,
        //ML thread
        [op = m_op, indexes](vlc_medialibrary_t* ml, Ctx& ctx)
        {
            if (indexes.isEmpty())
                return;

            auto sortedIndexes = indexes;
            std::sort(sortedIndexes.begin(), sortedIndexes.end());

            struct Range
            {
                int low, high; // [low, high] (all inclusive)
            };

            QVector<Range> ranges;
            ranges.push_back(Range {sortedIndexes[0], sortedIndexes[0]});
            const int MAX_DIFFERENCE = 4;
            for (const auto index : sortedIndexes)
            {
                if ((index - ranges.back().high) < MAX_DIFFERENCE)
                    ranges.back().high = index;
                else
                    ranges.push_back(Range {index, index});
            }

            ctx.items.resize(indexes.size());

            vlc_ml_query_params_t queryParam = op->getQueryParams();
            for (const auto range : ranges)
            {
                queryParam.i_offset = range.low;
                queryParam.i_nbResults = range.high - range.low + 1;
                auto data = op->load(ml, &queryParam);
                for (int i = 0; i < indexes.size(); ++i)
                {
                    const auto targetIndex = indexes[i];
                    if (targetIndex >= range.low && targetIndex <= range.high)
                    {
                        ctx.items.at(i) = std::move(data.at(targetIndex - range.low));
                    }
                }
            }
        },
        // UI thread
        [cb](quint64 id, Ctx &ctx) {
            cb(id, ctx.items);
        });
}


size_t MLListCacheLoader::loadItemByIdTask(MLItemId itemId, std::function<void (size_t, ItemType&&)> cb) const
{
    struct Ctx {
        ItemType item;
    };
    return m_medialib->runOnMLThread<Ctx>(this,
        //ML thread
        [itemId, op = m_op](vlc_medialibrary_t* ml, Ctx& ctx) {
            ctx.item = op->loadItemById(ml, itemId);
        },
        //UI thread
        [this, cb](qint64 taskId, Ctx& ctx) {
            if (!ctx.item)
                return;
            cb(taskId, std::move(ctx.item));
        });
}

MLListCacheLoader::MLOp::MLOp(MLItemId parentId, QString searchPattern, vlc_ml_sorting_criteria_t sort, bool sort_desc)
    : m_parent(parentId)
    , m_searchPattern(searchPattern.toUtf8())
    , m_sort(sort)
    , m_sortDesc(sort_desc)
{
}

vlc_ml_query_params_t MLListCacheLoader::MLOp::getQueryParams(size_t offset, size_t limit) const
{
    vlc_ml_query_params_t params;
    params.psz_pattern = m_searchPattern.isNull()
                             ? nullptr
                             : m_searchPattern.constData();
    params.i_nbResults = limit;
    params.i_offset = offset;
    params.i_sort = m_sort;
    params.b_desc = m_sortDesc;
    return params;
}
