#include <Geode/Geode.hpp>
#include <Geode/modify/LevelSearchLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/GJSearchObject.hpp>
#include <Geode/utils/cocos.hpp>
#include <random>
#include <chrono>
#include <map>
#include <string>
#include <sstream>

using namespace geode::prelude;

static bool g_enteredViaRandom = false;
static int g_cachedMaxOnlineID = 0;
static std::map<std::string, int> g_filterCache;

static int generateRandomInt(int min, int max) {
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(min, max);
    return dist(rng);
}

$execute{
    g_filterCache = Mod::get()->getSavedValue<std::map<std::string, int>>("filter_cache");
}

std::string getSearchKey(GJSearchObject* obj) {
    if (!obj) return "";

    std::stringstream ss;
    ss << "Diff:" << obj->m_difficulty
        << "_Len:" << obj->m_length
        << "_Star:" << (int)obj->m_starFilter
        << "_NoStar:" << (int)obj->m_noStarFilter
        << "_Feat:" << (int)obj->m_featuredFilter
        << "_Epic:" << (int)obj->m_epicFilter
        << "_Leg:" << (int)obj->m_legendaryFilter
        << "_Myth:" << (int)obj->m_mythicFilter
        << "_Song:" << obj->m_songID
        << "_Custom:" << (int)obj->m_customSongFilter;

    if (!obj->m_searchQuery.empty()) {
        ss << "_Q:" << obj->m_searchQuery;
    }
    return ss.str();
}

class RandomSearchDelegate : public CCObject, public LevelManagerDelegate {
public:
    using SuccessCallback = std::function<void(GJSearchObject*, CCArray*)>;
    using FailCallback = std::function<void(GJSearchObject*)>;

    SuccessCallback m_onSuccess;
    FailCallback m_onFail;
    GJSearchObject* m_currentSearchObj = nullptr;

    static RandomSearchDelegate* create(SuccessCallback onSuccess, FailCallback onFail) {
        auto ret = new RandomSearchDelegate();
        ret->m_onSuccess = onSuccess;
        ret->m_onFail = onFail;
        ret->autorelease();
        return ret;
    }

    void invalidate() {
        m_onSuccess = nullptr;
        m_onFail = nullptr;
    }

    void loadLevelsFinished(CCArray* levels, const char* page) override {
        if (m_onSuccess && m_currentSearchObj) m_onSuccess(m_currentSearchObj, levels);
    }

    void loadLevelsFailed(const char* page) override {
        if (m_onFail && m_currentSearchObj) m_onFail(m_currentSearchObj);
    }

    void setupSearch(GJSearchObject* obj) {
        CC_SAFE_RELEASE(m_currentSearchObj);
        m_currentSearchObj = obj;
        CC_SAFE_RETAIN(m_currentSearchObj);
    }

    ~RandomSearchDelegate() {
        CC_SAFE_RELEASE(m_currentSearchObj);
    }
};

class $modify(RandomLevelInfoLayer, LevelInfoLayer) {
    void onBack(CCObject * sender) {
        if (g_enteredViaRandom) {
            g_enteredViaRandom = false;
            auto scene = LevelSearchLayer::scene(0);
            CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(0.5f, scene));
            return;
        }
        LevelInfoLayer::onBack(sender);
    }
};

class $modify(RandomLevelSearch, LevelSearchLayer) {
    enum class RandomMode { None, Smart, Chaos };

    enum class SmartPhase {
        Idle,
        Phase1_CheckTotal,
        Phase2_CachePeek,
        Phase2b_CacheNext,
        Phase3_GlitchCheck,
        Phase4_BinarySearch,
        Phase5_CalcExact,
        Phase6_FetchTarget
    };

    struct Fields {
        RandomMode m_currentMode = RandomMode::None;
        SmartPhase m_smartPhase = SmartPhase::Idle;
        LoadingCircle* m_loadingCircle = nullptr;
        Ref<RandomSearchDelegate> m_delegate = nullptr;

        bool m_usingFilters = false;
        std::string m_currentFilterKey;

        int m_searchLow = 0;
        int m_searchHigh = 0;
        int m_foundMaxPage = 0;

        int m_targetPage = 0;
        int m_targetSlot = 0;

        int m_retryCount = 0;
        bool m_isFetchingLatest = false;
    };

    bool init(int p0) {
        if (!LevelSearchLayer::init(p0)) return false;
        g_enteredViaRandom = false;

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        auto menu = CCMenu::create();
        menu->setPosition({ winSize.width - 45.f, winSize.height - 215.f });
        menu->setID("random-level-menu"_spr);
        menu->setLayout(RowLayout::create()->setGap(5.f)->setAxisReverse(true));
        this->addChild(menu);

        auto createIcon = [](std::string spriteName, std::string fallbackText, float scale, CCPoint offset = { 0.0f, 0.0f }) -> CCNode* {
            CCNode* content = CCSprite::create(spriteName.c_str());

            if (!content) {
                auto lbl = CCLabelBMFont::create(fallbackText.c_str(), "bigFont.fnt");
                lbl->setScale(0.5f);
                return lbl;
            }

            content->setScale(scale);
            float containerSize = 40.0f;
            auto container = CCNode::create();
            container->setContentSize({ containerSize, containerSize });
            container->setAnchorPoint({ 0.5f, 0.5f });
            content->setAnchorPoint({ 0.5f, 0.5f });
            content->setPosition({ (containerSize / 2) + offset.x, (containerSize / 2) + offset.y });
            container->addChild(content);
            return container;
            };

        CCNode* chaosNode = createIcon("rng_512.png"_spr, "RNG", 0.4f);
        auto chaosBtnSprite = CircleButtonSprite::create(chaosNode, CircleBaseColor::Green, CircleBaseSize::Medium);
        if (chaosBtnSprite) {
            chaosBtnSprite->setScale(0.8f);
            auto btnChaos = CCMenuItemSpriteExtra::create(chaosBtnSprite, this, menu_selector(RandomLevelSearch::onChaosRandom));
            menu->addChild(btnChaos);
        }

        CCNode* smartNode = createIcon("smart_random_512.png"_spr, "?", 0.375f, { 1.0f, 1.0f });
        auto smartBtnSprite = CircleButtonSprite::create(smartNode, CircleBaseColor::Green, CircleBaseSize::Medium);
        if (smartBtnSprite) {
            smartBtnSprite->setScale(0.8f);
            auto btnSmart = CCMenuItemSpriteExtra::create(smartBtnSprite, this, menu_selector(RandomLevelSearch::onSmartRandom));
            menu->addChild(btnSmart);
        }
        menu->updateLayout();
        return true;
    }

    void showLoading() {
        if (!m_fields->m_loadingCircle) {
            m_fields->m_loadingCircle = LoadingCircle::create();
            m_fields->m_loadingCircle->setParentLayer(this);
            m_fields->m_loadingCircle->setFade(true);
            m_fields->m_loadingCircle->show();
        }
    }

    void onChaosRandom(CCObject * sender) {
        if (GJAccountManager::sharedState()->m_accountID <= 0) return;
        if (m_fields->m_currentMode != RandomMode::None) return;

        showLoading();
        this->scheduleOnce(schedule_selector(RandomLevelSearch::deferredChaosSearch), 0.0f);
    }

    void deferredChaosSearch(float) {
        m_fields->m_currentMode = RandomMode::Chaos;
        m_fields->m_isFetchingLatest = (g_cachedMaxOnlineID == 0);
        log::info("[Random] Mode: CHAOS");

        this->startRandomSearch();
        this->attemptSearch();
    }

    void onSmartRandom(CCObject * sender) {
        if (GJAccountManager::sharedState()->m_accountID <= 0) return;
        if (m_fields->m_currentMode != RandomMode::None) return;

        showLoading();
        this->scheduleOnce(schedule_selector(RandomLevelSearch::deferredSmartSearch), 0.0f);
    }

    void deferredSmartSearch(float) {
        m_fields->m_currentMode = RandomMode::Smart;

        auto testObj = this->getSearchObject(SearchType::Search, "");
        if (!testObj) {
            this->abortSearch("Failed to create search object.");
            return;
        }

        auto isSongFilterActive = [](GJSearchObject* o) {
            if (o->m_customSongFilter != 0) return true;
            return o->m_songID > 1;
            };

        log::info("[Random] Smart Search Started.");

        m_fields->m_usingFilters =
            (testObj->m_starFilter ||
                testObj->m_noStarFilter ||
                testObj->m_difficulty != "-" ||
                testObj->m_length != "-" ||
                testObj->m_completedFilter ||
                testObj->m_uncompletedFilter ||
                testObj->m_featuredFilter ||
                testObj->m_epicFilter ||
                testObj->m_legendaryFilter ||
                testObj->m_mythicFilter ||
                isSongFilterActive(testObj) ||
                !testObj->m_searchQuery.empty());

        m_fields->m_retryCount = 0;
        m_fields->m_currentFilterKey = "";

        if (!m_fields->m_usingFilters) {
            log::info("[Random] Smart Mode (No Filters) -> Switching to Chaos Logic.");
            m_fields->m_currentMode = RandomMode::Chaos;
            m_fields->m_isFetchingLatest = (g_cachedMaxOnlineID == 0);
            this->startRandomSearch();
            this->attemptSearch();
            return;
        }

        m_fields->m_currentFilterKey = getSearchKey(testObj);

        if (g_filterCache.count(m_fields->m_currentFilterKey)) {
            int cachedPage = g_filterCache[m_fields->m_currentFilterKey];
            if (cachedPage == 501 || cachedPage >= 1000) {
                log::info("[Random] Cache is {} (Infinite/Max). Verifying Glitch Check...", cachedPage);
                m_fields->m_smartPhase = SmartPhase::Phase3_GlitchCheck;
            }
            else {
                log::info("[Random] Cache HIT! Peeking at Page {}...", cachedPage);
                m_fields->m_foundMaxPage = cachedPage;
                m_fields->m_smartPhase = SmartPhase::Phase2_CachePeek;
            }
        }
        else {
            log::info("[Random] Cache MISS. Starting Fresh Discovery.");
            m_fields->m_smartPhase = SmartPhase::Phase1_CheckTotal;
        }

        this->startRandomSearch();
        this->attemptSearch();
    }

    void startRandomSearch() {
        showLoading();
        m_fields->m_delegate = RandomSearchDelegate::create(
            [this](GJSearchObject* obj, CCArray* levels) { this->onRandomSuccess(obj, levels); },
            [this](GJSearchObject* obj) { this->onRandomFailed(obj); }
        );
    }

    void attemptSearch() {
        if (m_fields->m_currentMode == RandomMode::None) return;

        bool isIdGen = (m_fields->m_currentMode == RandomMode::Chaos ||
            (m_fields->m_currentMode == RandomMode::Smart && !m_fields->m_usingFilters));

        if (isIdGen) {
            if (m_fields->m_isFetchingLatest) {
                auto searchObj = GJSearchObject::create(SearchType::Recent);
                m_fields->m_delegate->setupSearch(searchObj);
                GameLevelManager::sharedState()->m_levelManagerDelegate = m_fields->m_delegate;
                GameLevelManager::sharedState()->getOnlineLevels(searchObj);
                return;
            }
            int randomID = this->getRandomID();
            auto searchObj = GJSearchObject::create(SearchType::Search, std::to_string(randomID));
            m_fields->m_delegate->setupSearch(searchObj);
            GameLevelManager::sharedState()->m_levelManagerDelegate = m_fields->m_delegate;
            GameLevelManager::sharedState()->getOnlineLevels(searchObj);
            return;
        }

        auto searchObj = this->getSearchObject(SearchType::Search, "");

        switch (m_fields->m_smartPhase) {
        case SmartPhase::Phase1_CheckTotal:
            searchObj->m_page = 0;
            log::info("[Random] Phase 1: Checking Page 0...");
            break;
        case SmartPhase::Phase2_CachePeek:
            searchObj->m_page = m_fields->m_foundMaxPage;
            log::info("[Random] Phase 2: Peeking Cache (Page {})...", m_fields->m_foundMaxPage);
            break;
        case SmartPhase::Phase2b_CacheNext:
            searchObj->m_page = m_fields->m_foundMaxPage + 1;
            log::info("[Random] Phase 2b: Checking Next Page (Page {})...", searchObj->m_page);
            break;
        case SmartPhase::Phase3_GlitchCheck:
            searchObj->m_page = 1000;
            log::info("[Random] Phase 3: Glitch Check (Page 1000)...");
            break;
        case SmartPhase::Phase4_BinarySearch: {
            int mid = m_fields->m_searchLow + (m_fields->m_searchHigh - m_fields->m_searchLow) / 2;
            log::info("[Random] Phase 4: Binary Search [{}-{}] -> Probing {}",
                m_fields->m_searchLow, m_fields->m_searchHigh, mid);
            searchObj->m_page = mid;
            break;
        }
        case SmartPhase::Phase5_CalcExact:
            searchObj->m_page = m_fields->m_foundMaxPage;
            log::info("[Random] Phase 5: Calculating exact count on Page {}", m_fields->m_foundMaxPage);
            break;
        case SmartPhase::Phase6_FetchTarget:
            searchObj->m_page = m_fields->m_targetPage;
            log::info("[Random] Phase 6: Fetching Target Page {}", m_fields->m_targetPage);
            break;
        default: break;
        }

        m_fields->m_delegate->setupSearch(searchObj);
        GameLevelManager::sharedState()->m_levelManagerDelegate = m_fields->m_delegate;
        GameLevelManager::sharedState()->getOnlineLevels(searchObj);
    }

    void onRandomSuccess(GJSearchObject * searchObj, CCArray * levels) {
        this->unschedule(schedule_selector(RandomLevelSearch::delayedRetry));

        bool isIdGen = (m_fields->m_currentMode == RandomMode::Chaos ||
            (m_fields->m_currentMode == RandomMode::Smart && !m_fields->m_usingFilters));

        if (isIdGen) {
            if (m_fields->m_isFetchingLatest) {
                m_fields->m_isFetchingLatest = false;
                if (levels && levels->count() > 0) {
                    if (auto l = typeinfo_cast<GJGameLevel*>(levels->objectAtIndex(0))) {
                        if (l->m_levelID > 0) g_cachedMaxOnlineID = l->m_levelID;
                    }
                }
                if (g_cachedMaxOnlineID == 0) g_cachedMaxOnlineID = 100000000;
                this->scheduleOnce(schedule_selector(RandomLevelSearch::delayedRetry), 0.5f);
                return;
            }
            if (levels && levels->count() > 0) this->pickRandomLevel(levels);
            else this->scheduleOnce(schedule_selector(RandomLevelSearch::delayedRetry), 0.5f);
            return;
        }

        int count = (levels) ? levels->count() : 0;
        float safeDelay = 0.5f;
        float searchDelay = 0.75f;

        switch (m_fields->m_smartPhase) {
        case SmartPhase::Phase1_CheckTotal: {
            if (count == 0) { this->abortSearch("[Random] No levels found."); return; }
            int total = searchObj->m_total;
            if (total > 0 && total < 9990) {
                log::info("[Random] Trusted Total: {}", total);
                int maxPage = (total - 1) / 10;
                int lastPageCount = (total - 1) % 10 + 1;
                this->prepareTarget(maxPage, lastPageCount);
                return;
            }
            m_fields->m_smartPhase = SmartPhase::Phase3_GlitchCheck;
            this->scheduleOnce(schedule_selector(RandomLevelSearch::delayedRetry), safeDelay);
            break;
        }

        case SmartPhase::Phase2_CachePeek: {
            if (count < 10 && count > 0) {
                log::info("[Random] Cache Peek: Page not full. Done.");
                this->prepareTarget(m_fields->m_foundMaxPage, count);
            }
            else if (count == 10) {
                m_fields->m_smartPhase = SmartPhase::Phase2b_CacheNext;
                this->scheduleOnce(schedule_selector(RandomLevelSearch::delayedRetry), safeDelay);
            }
            else {
                log::info("[Random] Cache Peek: Page empty. Searching backwards.");
                m_fields->m_searchLow = 0;
                m_fields->m_searchHigh = m_fields->m_foundMaxPage;
                m_fields->m_smartPhase = SmartPhase::Phase4_BinarySearch;
                this->scheduleOnce(schedule_selector(RandomLevelSearch::delayedRetry), searchDelay);
            }
            break;
        }

        case SmartPhase::Phase2b_CacheNext: {
            if (count == 0) {
                log::info("[Random] Next page empty. Original cache was end (10 items).");
                this->prepareTarget(m_fields->m_foundMaxPage, 10);
            }
            else if (count < 10) {
                log::info("[Random] New levels found (Count {}). Updated end.", count);
                this->prepareTarget(m_fields->m_foundMaxPage + 1, count);
            }
            else {
                if (searchObj->m_page >= 1000) {
                    log::info("[Random] Hit Page 1000+ via Cache (Infinite). Capping at 501.");
                    this->prepareTarget(501, 10);
                    return;
                }
                log::info("[Random] Next page full. Expanding search to Limit (1000).");
                m_fields->m_searchLow = m_fields->m_foundMaxPage + 1;
                m_fields->m_searchHigh = 1000;
                m_fields->m_smartPhase = SmartPhase::Phase4_BinarySearch;
                this->scheduleOnce(schedule_selector(RandomLevelSearch::delayedRetry), searchDelay);
            }
            break;
        }

        case SmartPhase::Phase3_GlitchCheck: {
            if (count > 0) {
                log::info("[Random] Glitch Detected (Page 1000 has levels). Capping 501.");
                this->prepareTarget(501, 10);
            }
            else {
                log::info("[Random] Finite Mode. Binary Search.");
                m_fields->m_searchLow = 0;
                m_fields->m_searchHigh = 1000;
                m_fields->m_smartPhase = SmartPhase::Phase4_BinarySearch;
                this->scheduleOnce(schedule_selector(RandomLevelSearch::delayedRetry), searchDelay);
            }
            break;
        }

        case SmartPhase::Phase4_BinarySearch: {
            int searchedPage = searchObj->m_page;
            if (count > 0) {
                if (searchedPage >= 1000) {
                    log::info("[Random] Binary Search hit 1000+ (Infinite). Capping at 501.");
                    this->prepareTarget(501, 10);
                    return;
                }
                m_fields->m_searchLow = searchedPage;
            }
            else {
                m_fields->m_searchHigh = searchedPage;
            }

            if (m_fields->m_searchHigh - m_fields->m_searchLow <= 1) {
                m_fields->m_foundMaxPage = m_fields->m_searchLow;
                m_fields->m_smartPhase = SmartPhase::Phase5_CalcExact;
            }
            this->scheduleOnce(schedule_selector(RandomLevelSearch::delayedRetry), searchDelay);
            break;
        }

        case SmartPhase::Phase5_CalcExact: {
            if (count == 0) {
                if (m_fields->m_foundMaxPage > 0) {
                    m_fields->m_foundMaxPage--;
                    this->scheduleOnce(schedule_selector(RandomLevelSearch::delayedRetry), safeDelay);
                    return;
                }
                this->abortSearch("[Random] Final page empty."); return;
            }
            this->prepareTarget(m_fields->m_foundMaxPage, count);
            break;
        }

        case SmartPhase::Phase6_FetchTarget: {
            int index = m_fields->m_targetSlot;

            if (count == 0) {
                log::info("[Random] Fast Path failed (Empty Page). Invalidating cache.");
                g_filterCache.erase(m_fields->m_currentFilterKey);
                Mod::get()->setSavedValue("filter_cache", g_filterCache);
                m_fields->m_smartPhase = SmartPhase::Phase1_CheckTotal;
                this->scheduleOnce(schedule_selector(RandomLevelSearch::delayedRetry), safeDelay);
                return;
            }

            if (index >= count) index = count - 1;

            if (index >= 0) {
                auto lvl = typeinfo_cast<GJGameLevel*>(levels->objectAtIndex(index));
                if (lvl) this->openLevelPage(lvl);
                else this->abortSearch("[Random] Level object was null.");
            }
            else {
                this->abortSearch("[Random] Invalid slot index.");
            }
            break;
        }
        }
    }

    void onRandomFailed(GJSearchObject * searchObj) {
        this->unschedule(schedule_selector(RandomLevelSearch::delayedRetry));

        if (m_fields->m_smartPhase == SmartPhase::Phase2_CachePeek) {
            log::info("[Random] Cache Peek Failed. Searching Backwards.");
            m_fields->m_searchLow = 0;
            m_fields->m_searchHigh = m_fields->m_foundMaxPage;
            m_fields->m_smartPhase = SmartPhase::Phase4_BinarySearch;
            this->scheduleOnce(schedule_selector(RandomLevelSearch::delayedRetry), 0.75f);
            return;
        }

        if (m_fields->m_smartPhase == SmartPhase::Phase2b_CacheNext) {
            log::info("[Random] Next page failed. Using Cached Page (10 items).");
            this->prepareTarget(m_fields->m_foundMaxPage, 10);
            return;
        }

        if (m_fields->m_smartPhase == SmartPhase::Phase3_GlitchCheck) {
            m_fields->m_searchLow = 0;
            m_fields->m_searchHigh = 1000;
            m_fields->m_smartPhase = SmartPhase::Phase4_BinarySearch;
            this->scheduleOnce(schedule_selector(RandomLevelSearch::delayedRetry), 0.75f);
            return;
        }

        if (m_fields->m_smartPhase == SmartPhase::Phase4_BinarySearch) {
            int failedPage = searchObj->m_page;
            m_fields->m_searchHigh = failedPage;
            if (m_fields->m_searchHigh - m_fields->m_searchLow <= 1) {
                m_fields->m_foundMaxPage = m_fields->m_searchLow;
                m_fields->m_smartPhase = SmartPhase::Phase5_CalcExact;
            }
            this->scheduleOnce(schedule_selector(RandomLevelSearch::delayedRetry), 0.75f);
            return;
        }

        if (m_fields->m_currentMode == RandomMode::Chaos) {
            this->scheduleOnce(schedule_selector(RandomLevelSearch::delayedRetry), 0.5f);
            return;
        }

        m_fields->m_retryCount++;
        if (m_fields->m_retryCount > 5) {
            this->abortSearch("[Random] Connection Failed or Timed Out.");
            return;
        }
        this->scheduleOnce(schedule_selector(RandomLevelSearch::delayedRetry), 1.0f);
    }

    void prepareTarget(int maxPageInclusive, int levelsOnMaxPage) {
        long long totalLevels = ((long long)maxPageInclusive * 10) + levelsOnMaxPage;

        if (m_fields->m_usingFilters) {
            g_filterCache[m_fields->m_currentFilterKey] = maxPageInclusive;
            Mod::get()->setSavedValue("filter_cache", g_filterCache);
        }

        int globalIndex = generateRandomInt(0, (int)totalLevels - 1);

        m_fields->m_targetPage = globalIndex / 10;
        m_fields->m_targetSlot = globalIndex % 10;

        m_fields->m_smartPhase = SmartPhase::Phase6_FetchTarget;
        this->scheduleOnce(schedule_selector(RandomLevelSearch::delayedRetry), 0.1f);
    }

    void abortSearch(std::string reason) {
        log::error("{}", reason);
        this->stopSearchLogic();

        auto alert = FLAlertLayer::create("Random Search", reason.c_str(), "OK");
        alert->show();
    }

    void delayedRetry(float) { this->attemptSearch(); }

    void pickRandomLevel(CCArray * levels) {
        if (!levels || levels->count() == 0) { this->abortSearch("No levels found."); return; }
        int slot = generateRandomInt(0, levels->count() - 1);

        this->openLevelPage(typeinfo_cast<GJGameLevel*>(levels->objectAtIndex(slot)));
    }

    void openLevelPage(GJGameLevel * level) {
        if (!level) return;

        log::info("=========================================");
        log::info("[Random] CHOSEN LEVEL:");
        log::info("[Random] Name: {}", level->m_levelName);
        log::info("[Random] ID:   {}", level->m_levelID.value());

        if (m_fields->m_currentMode == RandomMode::Chaos) {
            log::info("[Random] Max Recents ID: {}", g_cachedMaxOnlineID);
        }
        else {
            log::info("[Random] Page: {}", m_fields->m_targetPage + 1);
            log::info("[Random] Slot: {}", m_fields->m_targetSlot + 1);
        }
        log::info("=========================================");

        this->stopSearchLogic();
        g_enteredViaRandom = true;
        auto saved = GameLevelManager::sharedState()->getSavedLevel(level->m_levelID);
        auto scene = LevelInfoLayer::scene(saved ? saved : level, false);
        CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(0.5f, scene));
    }

    int getRandomID() {
        int max = (g_cachedMaxOnlineID > 0) ? g_cachedMaxOnlineID : 100000000;
        return generateRandomInt(128, max);
    }

    void stopSearchLogic() {
        m_fields->m_currentMode = RandomMode::None;
        if (m_fields->m_loadingCircle) {
            m_fields->m_loadingCircle->fadeAndRemove();
            m_fields->m_loadingCircle = nullptr;
        }

        if (m_fields->m_delegate) {
            m_fields->m_delegate->invalidate();
        }

        if (GameLevelManager::sharedState()->m_levelManagerDelegate == m_fields->m_delegate) {
            GameLevelManager::sharedState()->m_levelManagerDelegate = nullptr;
        }

        m_fields->m_delegate = nullptr;

        this->unschedule(schedule_selector(RandomLevelSearch::delayedRetry));
        this->unschedule(schedule_selector(RandomLevelSearch::deferredSmartSearch));
        this->unschedule(schedule_selector(RandomLevelSearch::deferredChaosSearch));
    }

    void onExit() {
        this->stopSearchLogic();
        LevelSearchLayer::onExit();
    }
};