#pragma execution_character_set("utf-8")
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <unordered_map>
#include <set>
#include "MinHook.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_win32.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// ============================================================
//  СЕКЦИЯ 1: Глобальные переменные и тумблеры
// ============================================================

char g_gameVersion[32] = {};

// --- Тумблеры функций ---
bool godMode          = false;  // Режим бога (блок урона)
bool coinMultEnabled  = false;  // Множитель монет
bool forceDropEnabled = false;  // Гарантированный дроп предметов
bool distanceBonusEnabled = false;  // ← Бонус Distance
bool logEnabled       = false;   // Логирование в консоль
bool consoleVisible   = false;   // Видимость окна консоли
// Глобальная:
bool speedBonusEnabled = false;
bool attackBonusEnabled = false;
float speedBonus = 100.0f;
float attackBonus = 1000.0f;
// --- Множители ---
float coinMult     = 100.0f;
float multLevel     = 1000.0f;
float multCombat    = 1000.0f;
float multDistance  = 1000.0f;
float multMagic     = 1000.0f;

// --- Бонус Distance ---
float distanceBonus = 210.0f;   // ← фиксированное значение

// --- Кэш StatFile ---
void* g_playerStatFile   = nullptr;
float g_originalDistance = 0.0f;
bool  g_originalSaved    = false;

// --- Тумблеры опыта ---
bool expLevelEnabled    = true;
bool expCombatEnabled   = true;
bool expDistanceEnabled = true;
bool expMagicEnabled    = true;

// --- Адреса методов (заполняются в InitHooks) ---
uintptr_t addExpAddr            = 0;  // Stat.AddExp
uintptr_t getDmgAddr            = 0;  // Hero.GetDmg
uintptr_t applyHpAddr           = 0;  // CombatObject.ApplyHp
uintptr_t initCoinAddr          = 0;  // Coin.InitCoin
uintptr_t showCoinAddr          = 0;  // Coin.ShowCoin
uintptr_t createLootAddr        = 0;  // LootManager.CreateLoot
uintptr_t itemsToDropAddr       = 0;  // LootManager.ItemsToDrop
uintptr_t itemHasReachBagAddr   = 0;  // LootManager.ItemHasReachBag
uintptr_t drawItemsAddr         = 0;  // Enemy.DrawItems
uintptr_t lootItemActivationAddr = 0;  // LootItem.Activation
uintptr_t attackDoneAddr        = 0;  // ← HeroStats.AttackDone
    uintptr_t offsetBaseStats;
    uintptr_t offsetDistance;
    uintptr_t offsetSpeed;
    uintptr_t offsetAttack;

// --- Позиция лута ---
float lastLootPos[3] = {0, 0, 0};
bool  hasLootPos = false;

// --- Кэш множителя монет ---
std::unordered_map<void*, DWORD> coinLastMult;

// --- Защита от повторного форсирования дропа ---
std::set<void*> g_forcedItems;

// ============================================================
//  СЕКЦИЯ 2: Утилиты
// ============================================================
#define LOG(fmt, ...) do { if (logEnabled) printf(fmt, ##__VA_ARGS__); } while(0)

char* GetObjectName(void* __this) {
    static char buf[256];
    buf[0] = 0;
    void* strPtr = *(void**)((unsigned char*)__this + 0x18);
    if (!strPtr) return buf;
    unsigned char* str = (unsigned char*)strPtr;
    int len = *(int*)(str + 0x10);
    if (len <= 0 || len >= 256) return buf;
    for (int i = 0; i < len; i++)
        buf[i] = str[0x14 + i * 2];
    buf[len] = 0;
    return buf;
}

// ============================================================
//  СЕКЦИЯ 3: Typedef'ы
// ============================================================
HWND g_hConsole = nullptr;

// Для 1.0.0 (int)
typedef void (*AddExp_v10_t)(void*, int);
AddExp_v10_t orig_AddExp_v10 = nullptr;

// Для 1.4.18 (long long)
typedef void (*AddExp_v18_t)(void*, long long);
AddExp_v18_t orig_AddExp_v18 = nullptr;

// Для 1.0.0 (Hero.GetDmg)
typedef bool (*GetDmg_v10_t)(void* __this, int dmg, int damageType);
GetDmg_v10_t orig_GetDmg_v10 = nullptr;

// Для 1.4.18 (Hero.GetDmg) (3 параметра)
typedef bool (*GetDmg_v18_t)(void*, int, int, int*);
GetDmg_v18_t orig_GetDmg_v18 = nullptr;

typedef void (*ApplyHp_t)(void* __this, int h, bool showPopText);
ApplyHp_t orig_ApplyHp = nullptr;

typedef void (*InitCoin_t)(void* __this, void* ground, void* pouch);
InitCoin_t orig_InitCoin = nullptr;

typedef void (*ShowCoin_t)(void* __this, float angle, void* dir, void* pos, bool isBonus, void* glowColor);
ShowCoin_t orig_ShowCoin = nullptr;

typedef void (*CreateLoot_t)(void* __this, void* enemy);
CreateLoot_t orig_CreateLoot = nullptr;

typedef void (*ItemsToDrop_t)(void* __this, void* itemsData, void* dropRarity, float* center, float dropHeight);
ItemsToDrop_t orig_ItemsToDrop = nullptr;

typedef void (*ItemHasReachBag_t)(void* __this, void* itemData);
ItemHasReachBag_t orig_ItemHasReachBag = nullptr;

typedef void* (*DrawItems_t)(void* __this);
DrawItems_t orig_DrawItems = nullptr;

typedef void (*LootItemActivation_t)(void* __this, void* item, int rarity, float delayTime, float xDistance, float* startPos, float endHeight);
LootItemActivation_t orig_LootItemActivation = nullptr;

// ← НОВОЕ: HeroStats.AttackDone
typedef void (*AttackDone_t)(void* __this, bool isRanged);
AttackDone_t orig_AttackDone = nullptr;

typedef HRESULT(__stdcall* Present_t)(void* pSwapChain, UINT syncInterval, UINT flags);
Present_t orig_Present = nullptr;

// ============================================================
//  СЕКЦИЯ 4: Реализации хуков
// ============================================================

void Hook_AddExp_v10(void* __this, int exp) {
    char* name = GetObjectName(__this);
    if (strcmp(name, "Level") == 0 && expLevelEnabled)
        exp = (int)(exp * multLevel);
    else if (strcmp(name, "Combat") == 0 && expCombatEnabled)
        exp = (int)(exp * multCombat);
    else if (strcmp(name, "Distance") == 0 && expDistanceEnabled)
        exp = (int)(exp * multDistance);
    else if (strcmp(name, "Magic") == 0 && expMagicEnabled)
        exp = (int)(exp * multMagic);
   // LOG("[AddExp_v10] stat=%s exp=%I32d\n", name, exp);
    orig_AddExp_v10(__this, exp);
}

void Hook_AddExp_v18(void* __this, long long exp) {
    char* name = GetObjectName(__this);
    if (strcmp(name, "Level") == 0 && expLevelEnabled)
        exp = (long long)(exp * multLevel);
    else if (strcmp(name, "Combat") == 0 && expCombatEnabled)
        exp = (long long)(exp * multCombat);
    else if (strcmp(name, "Distance") == 0 && expDistanceEnabled)
        exp = (long long)(exp * multDistance);
    else if (strcmp(name, "Magic") == 0 && expMagicEnabled)
        exp = (long long)(exp * multMagic);
    // LOG("[AddExp_v18] stat=%s exp=%lld\n", name, exp);
    orig_AddExp_v18(__this, exp);
}

bool Hook_GetDmg_v10(void* __this, int dmg, int damageType) {
    if (godMode) {
       // LOG("[GetDmg_v10] GOD MODE: blocked %d dmg\n", dmg);
        return false;
    }
    return orig_GetDmg_v10(__this, dmg, damageType);
}

bool Hook_GetDmg_v18(void* __this, int dmg, int damageType, int* dealtDamage) {
    if (godMode) {
        if (dealtDamage) *dealtDamage = 0;
		      //  LOG("[GetDmg_v18] GOD MODE: blocked %d dmg\n", dmg);
        return false;
    }
    return orig_GetDmg_v18(__this, dmg, damageType, dealtDamage);
}

void Hook_ApplyHp(void* __this, int h, bool showPopText) {
    if (godMode && h < 0) {
       // LOG("[ApplyHp] GOD MODE: blocked %d hp\n", h);
        return;
    }
   // LOG("[ApplyHp] hp=%d showText=%d\n", h, showPopText);
    orig_ApplyHp(__this, h, showPopText);
}

void Hook_InitCoin(void* __this, void* ground, void* pouch) {
    int value = *(int*)((unsigned char*)__this + 0x20);
    // LOG("[InitCoin] value=%d\n", value);
    orig_InitCoin(__this, ground, pouch);
}

void Hook_ShowCoin(void* __this, float angle, void* dir, void* pos, bool isBonus, void* glowColor) {
    if (pos) {
        float* p = (float*)pos;
        lastLootPos[0] = p[0];
        lastLootPos[1] = p[1];
        lastLootPos[2] = p[2];
        hasLootPos = true;
    }

    int* valuePtr = (int*)((unsigned char*)__this + 0x20);
    if (coinMultEnabled) {
        DWORD now = GetTickCount();
        DWORD last = coinLastMult[__this];
        if (now - last > 200) {
            *valuePtr = (int)coinMult;
            coinLastMult[__this] = now;
        }
    } else {
        if (*valuePtr != 1) {
            *valuePtr = 1;
            coinLastMult.erase(__this);
        }
    }
    //LOG("[ShowCoin] value=%d pos=(%.1f,%.1f,%.1f)\n", *valuePtr,pos ? ((float*)pos)[0] : 0, pos ? ((float*)pos)[1] : 0, pos ? ((float*)pos)[2] : 0);
    orig_ShowCoin(__this, angle, dir, pos, isBonus, glowColor);
}

void Hook_CreateLoot(void* __this, void* enemy) {
    g_forcedItems.clear();
    hasLootPos = false;
    // g_originalSaved = false;   // ← УБЕРИ ЭТО
    // g_originalDistance = 0.0f; // ← И ЭТО
    LOG("[CreateLoot] enemy=%p\n", enemy);
    orig_CreateLoot(__this, enemy);
}

void Hook_ItemsToDrop(void* __this, void* itemsData, void* dropRarity, float* center, float dropHeight) {
    // LOG("[ItemsToDrop] center=(%.1f,%.1f,%.1f) dropHeight=%.1f\n",center ? center[0] : 0, center ? center[1] : 0, center ? center[2] : 0, dropHeight);
    orig_ItemsToDrop(__this, itemsData, dropRarity, center, dropHeight);
}

void Hook_ItemHasReachBag(void* __this, void* itemData) {
    if (itemData) {
        int id = *(int*)((unsigned char*)itemData + 0x10);
        int count = *(int*)((unsigned char*)itemData + 0x14);
        // LOG("[ItemHasReachBag] id=%d count=%d — ITEM ADDED!\n", id, count);
    }
    orig_ItemHasReachBag(__this, itemData);
}

void* Hook_DrawItems(void* __this) {
    void* result = orig_DrawItems(__this);
    if (result && forceDropEnabled) {
        void* itemsArray = *(void**)((unsigned char*)result + 0x10);
        int itemsCount = *(int*)((unsigned char*)result + 0x18);
        if (itemsArray && itemsCount > 0) {
            for (int i = 0; i < itemsCount; i++) {
                void* itemData = *(void**)((unsigned char*)itemsArray + 0x20 + i * 8);
                if (itemData && g_forcedItems.find(itemData) == g_forcedItems.end()) {
                    int count = *(int*)((unsigned char*)itemData + 0x14);
                    if (count <= 0) {
                        *(int*)((unsigned char*)itemData + 0x14) = 1;
                       // LOG("[DrawItems] FORCED count=1 for item %d\n", i);
                    }
                    g_forcedItems.insert(itemData);
                }
            }
        }
    }
    return result;
}

void Hook_LootItemActivation(void* __this, void* item, int rarity, float delayTime, float xDistance, float* startPos, float endHeight) {
    /*if (startPos)
        LOG("[LootItem.Activation] rarity=%d delay=%.2f pos=(%.1f,%.1f,%.1f)\n",rarity, delayTime, startPos[0], startPos[1], startPos[2]);
    else
        LOG("[LootItem.Activation] rarity=%d delay=%.2f pos=NULL\n", rarity, delayTime);
	*/
    orig_LootItemActivation(__this, item, rarity, delayTime, xDistance, startPos, endHeight);
}

// ---- HeroStats.AttackDone ----
// Бонус Distance (прибавка, только ranged)
void Hook_AttackDone(void* __this, bool isRanged) {
    LOG("[AttackDone] __this=%p isRanged=%d\n", __this, isRanged);

    if (__this == nullptr) {
        orig_AttackDone(__this, isRanged);
		LOG("[AttackDone] (__this == nullptr vyhod iz Hook_AttackDone");
        return;
    }


    if (strcmp(g_gameVersion, "1.0.0") == 0) {
        offsetBaseStats = 0x78;
    } else {
        offsetBaseStats = 0x90;
    }

    void* statFile = *(void**)((unsigned char*)__this + offsetBaseStats);
    LOG("[AttackDone] statFile=%p\n", statFile);

    if (statFile != nullptr) {
        // --- Distance ---
        float* distance = (float*)((unsigned char*)statFile + 0x30);
        LOG("[AttackDone] distance before=%.2f\n", *distance);

        if (distanceBonusEnabled && isRanged) {
            if (!g_originalSaved) {
                g_originalDistance = *distance;
                g_originalSaved = true;
                LOG("[AttackDone] saved original distance=%.2f\n", g_originalDistance);
            }
            *distance = g_originalDistance + distanceBonus;
            LOG("[AttackDone] distance after=%.2f\n", *distance);
        }
        else if (!distanceBonusEnabled && g_originalSaved) {
            *distance = g_originalDistance;
            LOG("[AttackDone] restored distance=%.2f\n", *distance);
            g_originalSaved = false;
            g_originalDistance = 0.0f;
        }

        // --- Speed ---
        if (speedBonusEnabled) {
            float* speed = (float*)((unsigned char*)statFile + 0x38);
            *speed = speedBonus;
            LOG("[AttackDone] speed set to %.1f\n", *speed);
        }

        // --- Attack ---
        if (attackBonusEnabled) {
            float* attack = (float*)((unsigned char*)statFile + 0x20);
            *attack = attackBonus;
            LOG("[AttackDone] attack set to %.0f\n", *attack);
        }
    }

    orig_AttackDone(__this, isRanged);
}

// ============================================================
//  СЕКЦИЯ 5: ImGui + DX11 Present
// ============================================================
ID3D11Device*            g_pd3dDevice = nullptr;
ID3D11DeviceContext*     g_pd3dContext = nullptr;
ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;
HWND g_hwnd = nullptr;
WNDPROC g_origWndProc = nullptr;

bool  g_menuOpen     = false;
bool  g_draggingIcon = false;
ImVec2 g_iconPos(50, 50);
float g_iconRadius = 30.0f;
bool  g_mouseOnIcon = false;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    if (msg == WM_KEYDOWN && wParam == VK_INSERT)
        g_menuOpen = !g_menuOpen;

    if (g_menuOpen || g_mouseOnIcon) {
        if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) return true;
        if (msg == WM_KEYDOWN || msg == WM_KEYUP ||
            msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) return true;
        if (msg == WM_INPUT) return true;
    }

    return CallWindowProc(g_origWndProc, hwnd, msg, wParam, lParam);
}

bool SwitchToggle(const char* label, bool* v) {
    bool changed = false;
    ImGui::PushID(label);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float h = 16.0f, w = 32.0f, r = h * 0.5f;

    ImGui::InvisibleButton(label, ImVec2(w + ImGui::CalcTextSize(label).x + 8.0f, h));
    bool clicked = ImGui::IsItemClicked();

    ImU32 bg = *v ? IM_COL32(40, 160, 40, 255) : IM_COL32(180, 40, 40, 255);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, r);
    float cx = *v ? p.x + w - r : p.x + r;
    dl->AddCircleFilled(ImVec2(cx, p.y + r), r - 2, IM_COL32(255, 255, 255, 255));
    dl->AddText(ImVec2(p.x + w + 6, p.y - 1), IM_COL32(255, 255, 255, 255), label);

    if (clicked) {
        *v = !*v;
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

HRESULT Hook_Present(IDXGISwapChain* swap, UINT sync, UINT flags) {
    static bool imguiInit = false;
    if (!imguiInit) {
        swap->GetDevice(__uuidof(ID3D11Device), (void**)&g_pd3dDevice);
        g_pd3dDevice->GetImmediateContext(&g_pd3dContext);

        DXGI_SWAP_CHAIN_DESC desc;
        swap->GetDesc(&desc);
        g_hwnd = desc.OutputWindow;
        if (!g_pd3dDevice || !g_pd3dContext || !g_hwnd)
            return orig_Present(swap, sync, flags);

        ID3D11Texture2D* pBackBuffer;
        swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer);
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();

        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.MouseDrawCursor = true;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        ImGui::StyleColorsDark();

        ImFont* font = io.Fonts->AddFontFromFileTTF(
            "C:\\Windows\\Fonts\\tahoma.ttf", 14.0f, nullptr,
            io.Fonts->GetGlyphRangesCyrillic());
        if (font) { io.FontDefault = font; printf("[ImGui] Cyrillic font loaded\n"); }
        fflush(stdout);

        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dContext);
        g_origWndProc = (WNDPROC)SetWindowLongPtr(g_hwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
        imguiInit = true;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    static bool lastConsoleVisible = true;
    if (consoleVisible != lastConsoleVisible) {
        ShowWindow(g_hConsole, consoleVisible ? SW_SHOW : SW_HIDE);
        lastConsoleVisible = consoleVisible;
    }

    ImGuiIO& io = ImGui::GetIO();

    // Иконка
    static bool dragging = false;
    static ImVec2 dragOffset(0, 0);
    ImVec2 mousePos = io.MousePos;
    float dx = mousePos.x - g_iconPos.x;
    float dy = mousePos.y - g_iconPos.y;
    float dist = sqrtf(dx * dx + dy * dy);
    g_mouseOnIcon = (dist <= g_iconRadius);

    if (io.MouseDown[0] && dist <= g_iconRadius && !dragging) {
        dragging = true;
        dragOffset = ImVec2(dx, dy);
    }
    if (dragging && io.MouseDown[0]) {
        g_iconPos.x = mousePos.x - dragOffset.x;
        g_iconPos.y = mousePos.y - dragOffset.y;
    }
    if (!io.MouseDown[0]) {
        if (dragging && dist <= g_iconRadius) g_menuOpen = !g_menuOpen;
        dragging = false;
    }

    g_iconPos.x = (g_iconPos.x < g_iconRadius) ? g_iconRadius : g_iconPos.x;
    g_iconPos.y = (g_iconPos.y < g_iconRadius) ? g_iconRadius : g_iconPos.y;
    g_iconPos.x = (g_iconPos.x > io.DisplaySize.x - g_iconRadius) ? io.DisplaySize.x - g_iconRadius : g_iconPos.x;
    g_iconPos.y = (g_iconPos.y > io.DisplaySize.y - g_iconRadius) ? io.DisplaySize.y - g_iconRadius : g_iconPos.y;

    ImDrawList* bg_dl = ImGui::GetBackgroundDrawList();
    bg_dl->AddCircleFilled(g_iconPos, g_iconRadius, IM_COL32(40, 100, 200, 255));
    bg_dl->AddCircle(g_iconPos, g_iconRadius, IM_COL32(255, 255, 255, 220), 32, 2.0f);
    const char* iconText = "H";
    ImVec2 textSize = ImGui::CalcTextSize(iconText);
    bg_dl->AddText(ImVec2(g_iconPos.x - textSize.x * 0.5f, g_iconPos.y - textSize.y * 0.5f),
                   IM_COL32(255, 255, 255, 255), iconText);

    // Меню
    if (g_menuOpen) {
        ImVec2 menuPos = g_iconPos;
        menuPos.x += g_iconRadius * 2 + 10;
        if (menuPos.x + 380 > io.DisplaySize.x) menuPos.x = g_iconPos.x - 380;
        if (menuPos.y + 400 > io.DisplaySize.y) menuPos.y = io.DisplaySize.y - 400;

        ImGui::SetNextWindowPos(menuPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(380, 400), ImGuiCond_FirstUseEver);
        ImGui::Begin("Cheat Menu", &g_menuOpen, ImGuiWindowFlags_NoCollapse);

        if (ImGui::BeginTabBar("##Tabs")) {
            if (ImGui::BeginTabItem("Основное")) {
                SwitchToggle("Режим бога", &godMode);
                ImGui::Separator();

                SwitchToggle("Множитель монет", &coinMultEnabled);
                ImGui::SameLine(220);
                ImGui::Text("+%.0f", coinMult);

                ImGui::Separator();
                SwitchToggle("Гарантированный дроп", &forceDropEnabled);

                // ← НОВОЕ: Бонус Distance (только тумблер)
                ImGui::Separator();
if (SwitchToggle("Бонус Distance (+210)", &distanceBonusEnabled)) {
    if (!distanceBonusEnabled) {
        g_originalSaved = false;
        g_originalDistance = 0.0f;
    }
}
SwitchToggle("Скорость атаки (speed)", &speedBonusEnabled);
ImGui::SameLine(220); ImGui::Text("%.1f", speedBonus);

SwitchToggle("Урон (attack)", &attackBonusEnabled);
ImGui::SameLine(220); ImGui::Text("%.0f", attackBonus);
                ImGui::Separator();
                ImGui::Text("Множители опыта:");
                ImGui::Separator();

                SwitchToggle("Опыт уровня", &expLevelEnabled);
                ImGui::SameLine(220); ImGui::Text("x%.0f", multLevel);
                SwitchToggle("Опыт боя", &expCombatEnabled);
                ImGui::SameLine(220); ImGui::Text("x%.0f", multCombat);
                SwitchToggle("Опыт дистанции", &expDistanceEnabled);
                ImGui::SameLine(220); ImGui::Text("x%.0f", multDistance);
                SwitchToggle("Опыт магии", &expMagicEnabled);
                ImGui::SameLine(220); ImGui::Text("x%.0f", multMagic);

                ImGui::Separator();
                ImGui::TextDisabled("Клик по иконке — открыть/закрыть");
                ImGui::TextDisabled("Перетаскивание — двигать иконку");
                ImGui::TextDisabled("INSERT — открыть/закрыть меню");
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Настройки")) {
                ImGui::Separator();
                ImGui::Text("Значения множителей:");
                ImGui::Separator();
                ImGui::InputFloat("Дроп монет+", &coinMult, 10.0f, 100.0f, "%.0f");
                ImGui::InputFloat("Опыт уровня x",    &multLevel,    100.0f, 1000.0f, "%.0f");
                ImGui::InputFloat("Опыт боя x",       &multCombat,   100.0f, 1000.0f, "%.0f");
                ImGui::InputFloat("Опыт дистанции x", &multDistance, 100.0f, 1000.0f, "%.0f");
                ImGui::InputFloat("Опыт магии x",     &multMagic,    100.0f, 1000.0f, "%.0f");
                ImGui::Separator();

                SwitchToggle("Логирование в консоль", &logEnabled);
                SwitchToggle("Показывать консоль", &consoleVisible);

                ImGui::Separator();
                ImGui::TextDisabled("Введите число + Enter");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
    }

    const char* verText = g_gameVersion[0] ? g_gameVersion : "unknown";
    char buf[64];
    snprintf(buf, sizeof(buf), "Версия v%s", verText);
    ImVec2 textSize1 = ImGui::CalcTextSize(buf);
    float pad = 8.0f;
    ImVec2 pos = ImVec2(pad, io.DisplaySize.y - textSize1.y - pad);
    ImGui::GetBackgroundDrawList()->AddText(pos, IM_COL32(255, 255, 255, 160), buf);

    ImGui::Render();
    g_pd3dContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    return orig_Present(swap, sync, flags);
}

// ============================================================
//  СЕКЦИЯ 6: Инициализация
// ============================================================
uintptr_t GetPresentVTable() {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = GetDesktopWindow();
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* pSwapChain = nullptr;
    ID3D11Device* pDevice = nullptr;
    D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &sd, &pSwapChain, &pDevice, nullptr, nullptr);

    void** vtable = *(void***)pSwapChain;
    uintptr_t addr = (uintptr_t)vtable[8];

    pSwapChain->Release();
    pDevice->Release();
    return addr;
}

void PrintGameVersionUnity() {
    HMODULE ga = GetModuleHandleA("GameAssembly.dll");
    if (!ga) return;

    auto resolve_icall = (void*(*)(const char*))GetProcAddress(ga, "il2cpp_resolve_icall");
    auto str_chars      = (const wchar_t*(*)(void*))GetProcAddress(ga, "il2cpp_string_chars");
    if (!resolve_icall || !str_chars) { printf("il2cpp exports not found\n"); return; }

    for (int attempt = 0; attempt < 20; attempt++) {
        __try {
            typedef void* (*getver_t)();
            getver_t get_version = (getver_t)resolve_icall("UnityEngine.Application::get_version");
            if (!get_version) { Sleep(500); continue; }

            void* str = get_version();
            if (!str) { Sleep(500); continue; }

            const wchar_t* chars = str_chars(str);
            if (!chars) { Sleep(500); continue; }

            size_t i = 0;
            for (; chars[i] && i < 31; i++) g_gameVersion[i] = (char)chars[i];
            g_gameVersion[i] = 0;

            if (g_gameVersion[0] != 0) { printf("Game version (Unity): %s\n", g_gameVersion); return; }
            Sleep(500);
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            Sleep(500);
        }
    }
}

void InitHooks() {
    AllocConsole();
    g_hConsole = GetConsoleWindow();
    freopen("CONOUT$", "w", stdout);
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("ExpHook loaded\n");

    PrintGameVersionUnity();
    if (g_gameVersion[0] == 0) {
        strcpy(g_gameVersion, "1.4.18");
    }

    if (MH_Initialize() != MH_OK) { printf("MH_Initialize FAILED\n"); return; }

    HMODULE gameAsm = GetModuleHandleA("GameAssembly.dll");
    if (!gameAsm) { printf("GameAssembly.dll NOT FOUND\n"); return; }
    printf("GameAssembly.dll base = %p\n", gameAsm);






// Stat.AddExp
// Hero.GetDmg
// CombatObject.ApplyHp
// Coin.InitCoin
// Coin.ShowCoin
// LootManager.CreateLoot
// LootManager.ItemsToDrop
// LootManager.ItemHasReachBag
// Enemy.DrawItems
 // LootItem.Activation
// ← HeroStats.AttackDone






    if (strcmp(g_gameVersion, "1.4.18") == 0) {
        addExpAddr            = (uintptr_t)gameAsm + 0x398B90;
		MH_CreateHook((void*)addExpAddr, &Hook_AddExp_v18, (void**)&orig_AddExp_v18);
        getDmgAddr            = (uintptr_t)gameAsm + 0x360EC0;
		MH_CreateHook((void*)getDmgAddr, &Hook_GetDmg_v18, (void**)&orig_GetDmg_v18);
        applyHpAddr           = (uintptr_t)gameAsm + 0x353D40;
        initCoinAddr          = (uintptr_t)gameAsm + 0x39BF00;
      //  showCoinAddr          = (uintptr_t)gameAsm + 0x39C6D0;
        createLootAddr        = (uintptr_t)gameAsm + 0x376620;
        itemsToDropAddr       = (uintptr_t)gameAsm + 0x377560;
        itemHasReachBagAddr   = (uintptr_t)gameAsm + 0x377410;
        drawItemsAddr         = (uintptr_t)gameAsm + 0x359D30;
        lootItemActivationAddr = (uintptr_t)gameAsm + 0x38F870;
        attackDoneAddr        = (uintptr_t)gameAsm + 0x38B290;  // ← НОВОЕ 1.4.18
    } else if (strcmp(g_gameVersion, "1.0.0") == 0) {
        addExpAddr            = (uintptr_t)gameAsm + 0x36FD30;
		MH_CreateHook((void*)addExpAddr, &Hook_AddExp_v10, (void**)&orig_AddExp_v10);
	     getDmgAddr = (uintptr_t)gameAsm + 0x339110; // Hero.GetDmg RVA 0x339110
        MH_CreateHook((void*)getDmgAddr, &Hook_GetDmg_v10, (void**)&orig_GetDmg_v10);
        applyHpAddr           = (uintptr_t)gameAsm + 0x32C610; // CombatObject.ApplyHp 0x32C610
        initCoinAddr          = (uintptr_t)gameAsm + 0x376550;
        showCoinAddr          = (uintptr_t)gameAsm + 0x376D80;
        createLootAddr        = (uintptr_t)gameAsm + 0x34ED00;
        itemsToDropAddr       = (uintptr_t)gameAsm + 0x34FC40;
        itemHasReachBagAddr   = (uintptr_t)gameAsm + 0x34FAF0;
        drawItemsAddr         = (uintptr_t)gameAsm + 0x332200;
        lootItemActivationAddr = (uintptr_t)gameAsm + 0x3673A0;
		attackDoneAddr        = (uintptr_t)gameAsm + 0x363FD0;  // ← НОВОЕ 1.0.0 0x363FD0
    } else if (strcmp(g_gameVersion, "1.4.12") == 0) {
		addExpAddr            = (uintptr_t)gameAsm + 0x385990;
		MH_CreateHook((void*)addExpAddr, &Hook_AddExp_v18, (void**)&orig_AddExp_v18);
        getDmgAddr            = (uintptr_t)gameAsm + 0x34DCA0;
		MH_CreateHook((void*)getDmgAddr, &Hook_GetDmg_v18, (void**)&orig_GetDmg_v18
		);
        applyHpAddr           = (uintptr_t)gameAsm + 0x340D20;
        initCoinAddr          = (uintptr_t)gameAsm + 0x388D00;
        showCoinAddr          = (uintptr_t)gameAsm + 0x3894D0;
        createLootAddr        = (uintptr_t)gameAsm + 0x363410;
        itemsToDropAddr       = (uintptr_t)gameAsm + 0x364350;
        itemHasReachBagAddr   = (uintptr_t)gameAsm + 0x364200;
        drawItemsAddr         = (uintptr_t)gameAsm + 0x346D20;
        lootItemActivationAddr = (uintptr_t)gameAsm + 0x37C640;
		attackDoneAddr        = (uintptr_t)gameAsm + 0x378080;  // ← НОВОЕ 1.4.12
    } else {
        addExpAddr            = (uintptr_t)gameAsm + 0x398B90;
		MH_CreateHook((void*)addExpAddr, &Hook_AddExp_v18, (void**)&orig_AddExp_v18);
        getDmgAddr            = (uintptr_t)gameAsm + 0x360EC0;
		MH_CreateHook((void*)getDmgAddr, &Hook_GetDmg_v18, (void**)&orig_GetDmg_v18
		);
        applyHpAddr           = (uintptr_t)gameAsm + 0x353D40;
        initCoinAddr          = (uintptr_t)gameAsm + 0x39BF00;
        showCoinAddr          = (uintptr_t)gameAsm + 0x39C6D0;
        createLootAddr        = (uintptr_t)gameAsm + 0x376620;
        itemsToDropAddr       = (uintptr_t)gameAsm + 0x377560;
        itemHasReachBagAddr   = (uintptr_t)gameAsm + 0x377410;
        drawItemsAddr         = (uintptr_t)gameAsm + 0x359D30;
        lootItemActivationAddr = (uintptr_t)gameAsm + 0x38F870;
        attackDoneAddr        = (uintptr_t)gameAsm + 0x38B290;  // ← НОВОЕ 1.4.18
    }

    if (addExpAddr) {

        MH_EnableHook((void*)addExpAddr);
        printf("CreateHook Stat.AddExp (addr=%p)\n", (void*)addExpAddr);
    }


 if (getDmgAddr) {

        MH_EnableHook((void*)getDmgAddr);
        printf("CreateHook Hero.GetDmg (addr=%p)\n", (void*)getDmgAddr);
 }	


    if (applyHpAddr) {
        MH_CreateHook((void*)applyHpAddr, &Hook_ApplyHp, (void**)&orig_ApplyHp);
        MH_EnableHook((void*)applyHpAddr);
        printf("CreateHook CombatObject.ApplyHp (addr=%p)\n", (void*)applyHpAddr);
    }

    if (initCoinAddr) {
        MH_CreateHook((void*)initCoinAddr, &Hook_InitCoin, (void**)&orig_InitCoin);
        MH_EnableHook((void*)initCoinAddr);
        printf("CreateHook Coin.InitCoin (addr=%p)\n", (void*)initCoinAddr);
    }

  if (showCoinAddr) {
        MH_CreateHook((void*)showCoinAddr, &Hook_ShowCoin, (void**)&orig_ShowCoin);
        MH_EnableHook((void*)showCoinAddr);
        printf("CreateHook Coin.ShowCoin (addr=%p)\n", (void*)showCoinAddr);
    }

    if (createLootAddr) {
        MH_CreateHook((void*)createLootAddr, &Hook_CreateLoot, (void**)&orig_CreateLoot);
        MH_EnableHook((void*)createLootAddr);
        printf("CreateHook LootManager.CreateLoot (addr=%p)\n", (void*)createLootAddr);
    }

    if (itemsToDropAddr) {
        MH_CreateHook((void*)itemsToDropAddr, &Hook_ItemsToDrop, (void**)&orig_ItemsToDrop);
        MH_EnableHook((void*)itemsToDropAddr);
        printf("CreateHook LootManager.ItemsToDrop (addr=%p)\n", (void*)itemsToDropAddr);
    }

    if (itemHasReachBagAddr) {
        MH_CreateHook((void*)itemHasReachBagAddr, &Hook_ItemHasReachBag, (void**)&orig_ItemHasReachBag);
        MH_EnableHook((void*)itemHasReachBagAddr);
        printf("CreateHook LootManager.ItemHasReachBag (addr=%p)\n", (void*)itemHasReachBagAddr);
    }

    if (drawItemsAddr) {
        MH_CreateHook((void*)drawItemsAddr, &Hook_DrawItems, (void**)&orig_DrawItems);
        MH_EnableHook((void*)drawItemsAddr);
        printf("CreateHook Enemy.DrawItems (addr=%p)\n", (void*)drawItemsAddr);
    }

    if (lootItemActivationAddr) {
        MH_CreateHook((void*)lootItemActivationAddr, &Hook_LootItemActivation, (void**)&orig_LootItemActivation);
        MH_EnableHook((void*)lootItemActivationAddr);
        printf("CreateHook LootItem.Activation (addr=%p)\n", (void*)lootItemActivationAddr);
    }

    // ← НОВОЕ: HeroStats.AttackDone
  if (attackDoneAddr) {
        MH_CreateHook((void*)attackDoneAddr, &Hook_AttackDone, (void**)&orig_AttackDone);
        MH_EnableHook((void*)attackDoneAddr);
        printf("CreateHook HeroStats.AttackDone (addr=%p)\n", (void*)attackDoneAddr);
    }

    uintptr_t presentAddr = GetPresentVTable();
    MH_CreateHook((void*)presentAddr, &Hook_Present, (void**)&orig_Present);
    MH_EnableHook((void*)presentAddr);
    printf("CreateHook Present (addr=%p)\n", (void*)presentAddr);
}

// ============================================================
//  СЕКЦИЯ 7: Точка входа
// ============================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            while (!GetModuleHandleA("GameAssembly.dll"))
                Sleep(100);
            Sleep(1000);
            InitHooks();
            return 0;
        }, nullptr, 0, nullptr);
    }
    return TRUE;
}