#include <Arduino.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <terminal_frotz.h>
#include "MenuDragon.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace {
TFT_eSPI display;

constexpr int keyboardSda = 32;
constexpr int keyboardScl = 25;
constexpr uint8_t keyboardAddress = 0x5F;
constexpr int sdSck = 18;
constexpr int sdMiso = 19;
constexpr int sdMosi = 23;
constexpr int sdSelect = 5;
constexpr uint8_t keyLeft = 0xB4;
constexpr uint8_t keyUp = 0xB5;
constexpr uint8_t keyDown = 0xB6;
constexpr uint8_t keyRight = 0xB7;
constexpr uint8_t keyEscape = 0x1B;

enum class Screen { Games, SavedGame, Settings, About, Story };
Screen screen = Screen::Games;
struct Game {
  std::string filename;
  std::string title;
  bool saved;
};
std::vector<Game> games;

struct Palette {
  const char* name;
  uint16_t foreground;
  uint16_t background;
  uint16_t accent;
};

enum AppleColor : uint8_t {
  AppleBlack, AppleMagenta, AppleDarkBlue, ApplePurple, AppleDarkGreen, AppleGray1,
  AppleMediumBlue, AppleLightBlue, AppleBrown, AppleOrange, AppleGray2, ApplePink,
  AppleLightGreen, AppleYellow, AppleAqua, AppleWhite,
};

constexpr uint16_t rgb565(uint32_t rgb) {
  return ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F);
}

// TFT approximations in Apple IIe Reference Manual Table 2-7 color-code order.
constexpr uint16_t appleColors[] = {
  rgb565(0x000000), rgb565(0xD0004F), rgb565(0x252080), rgb565(0xBB42CF),
  rgb565(0x00732B), rgb565(0x777777), rgb565(0x147BDE), rgb565(0x69ABFF),
  rgb565(0x8B4B00), rgb565(0xF77B19), rgb565(0xAAAAAA), rgb565(0xFF90AC),
  rgb565(0x65D92C), rgb565(0xF2E543), rgb565(0x4ADCC8), rgb565(0xFFFFFF),
};

constexpr uint8_t applePalette = 4;
constexpr Palette palettes[] = {
  {"Black on White", TFT_BLACK, TFT_WHITE, 0x52AA},
  {"White on Black", TFT_WHITE, TFT_BLACK, 0xAD55},
  {"Green on Black", 0x37E6, TFT_BLACK, 0x2448},
  {"Amber on Black", 0xFD20, TFT_BLACK, 0xA300},
  {"Apple II Lo-Res", appleColors[AppleWhite], appleColors[AppleBlack], appleColors[AppleAqua]},
};
constexpr size_t paletteCount = sizeof(palettes) / sizeof(palettes[0]);
constexpr uint8_t defaultPalette = 2;
uint8_t activePalette = defaultPalette;
size_t selectedPalette = defaultPalette;
bool showFilenames = false;
Preferences preferences;
bool preferencesReady = false;

const Palette& palette() {
  return palettes[activePalette];
}

bool appleTheme() {
  return activePalette == applePalette;
}

size_t selectedGame = 0;
size_t firstGame = 0;
size_t selectedAction = 0;
bool menuDirty = true;
bool menuLayoutDirty = true;
std::string menuMessage;
constexpr size_t menuRows = 9;
constexpr const char* savedActions[] = {"Continue", "New Game", "Back"};

constexpr int margin = 12;
constexpr int cellWidth = 12;
constexpr int lineHeight = 16;
constexpr size_t columns = 38;
constexpr size_t storyColumns = 27;
constexpr int menuTop = 86;
constexpr int menuRowHeight = 20;
constexpr size_t visibleRows = 17;
constexpr size_t historyRows = 96;
constexpr size_t upperRows = 3;

char history[historyRows][columns + 1] = {};
char upperWindow[upperRows][columns + 1] = {};
size_t currentRow = 0;
size_t currentColumn = 0;
size_t lineCount = 1;
size_t scrollOffset = 0;
bool transcriptDirty = true;
bool promptDirty = true;

bool cardMounted = false;
bool keyboardPresent = false;
bool keyboardChecked = false;
enum class Input { None, Command, SingleKey };
Input inputMode = Input::None;
FrotzStory frotzStory;
std::string activePath;
bool resumeFrotz = false;
char command[128] = {};
size_t commandLength = 0;

void clearTranscript() {
  currentRow = currentColumn = scrollOffset = 0;
  lineCount = 1;
  history[0][0] = '\0';
  transcriptDirty = true;
}

size_t upperHeight() {
  for (size_t row = upperRows; row > 0; --row) {
    for (size_t column = 0; column < columns; ++column) {
      if (upperWindow[row - 1][column] > ' ') {
        return row;
      }
    }
  }
  return 0;
}

void nextLine() {
  currentRow = (currentRow + 1) % historyRows;
  history[currentRow][0] = '\0';
  currentColumn = 0;
  if (lineCount < historyRows) {
    ++lineCount;
  }
}

void appendText(const char* text, size_t length) {
  transcriptDirty = true;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char ch = text[i];
    if (ch == '\n') {
      nextLine();
      continue;
    }
    if (ch < ' ') {
      continue;
    }

    if (currentColumn == columns) {
      if (ch == ' ') {
        nextLine();
        continue;
      }
      char* row = history[currentRow];
      char* space = strrchr(row, ' ');
      if (space && space != row) {
        char tail[columns + 1];
        strcpy(tail, space + 1);
        *space = '\0';
        nextLine();
        strcpy(history[currentRow], tail);
        currentColumn = strlen(tail);
      } else {
        nextLine();
      }
    }
    history[currentRow][currentColumn++] = ch <= '~' ? static_cast<char>(ch) : '?';
    history[currentRow][currentColumn] = '\0';
  }
}

void output(const char* text, size_t length) {
  appendText(text, length);
  Serial.write(reinterpret_cast<const uint8_t*>(text), length);
}

void drawTerminal() {
  const Palette& colors = palette();
  if (transcriptDirty) {
    display.fillRect(margin, margin, columns * cellWidth, visibleRows * lineHeight, colors.background);
    const size_t upper = upperHeight();
    if (upper) {
      display.setTextColor(appleTheme() ? appleColors[AppleWhite] : colors.background,
                           appleTheme() ? appleColors[AppleDarkBlue] : colors.foreground);
      for (size_t row = 0; row < upper; ++row) {
        display.setCursor(margin, margin + row * lineHeight);
        display.print(upperWindow[row]);
      }
    }
    display.setTextColor(colors.foreground, colors.background);
    const size_t count = min(visibleRows - upper, lineCount - scrollOffset);
    const size_t first = (currentRow + historyRows + 1 - scrollOffset - count) % historyRows;
    for (size_t i = 0; i < count; ++i) {
      display.setCursor(margin, margin + (i + upper) * lineHeight);
      display.print(history[(first + i) % historyRows]);
    }
    transcriptDirty = false;
  }

  if (promptDirty) {
    constexpr int promptY = margin + visibleRows * lineHeight + 8;
    display.fillRect(margin, promptY, columns * cellWidth, lineHeight, colors.background);
    display.setTextColor(appleTheme() ? appleColors[AppleLightGreen] : colors.foreground, colors.background);
    display.setCursor(margin, promptY);
    if (!keyboardPresent) {
      display.print("CONNECT CARDKB");
    } else if (scrollOffset) {
      display.print("DOWN: RETURN TO LATEST");
    } else if (inputMode == Input::SingleKey) {
      display.print("PRESS A KEY");
    } else if (inputMode == Input::Command) {
      const size_t promptWidth = columns - 3;
      const size_t offset = commandLength > promptWidth ? commandLength - promptWidth : 0;
      display.print(offset ? "< " : "> ");
      display.print(command + offset);
      display.print('_');
    }
    promptDirty = false;
  }
}

bool isStoryName(const char* name) {
  if (!name || name[0] == '.') {
    return false;
  }
  const char* extension = strrchr(name, '.');
  if (!extension) {
    return false;
  }
  return strcasecmp(extension, ".zblorb") == 0 ||
         strcasecmp(extension, ".zlb") == 0 ||
         strcasecmp(extension, ".dat") == 0 ||
         (strlen(extension) == 3 && (extension[1] == 'z' || extension[1] == 'Z') &&
          extension[2] >= '1' && extension[2] <= '8' && extension[2] != '6');
}

const std::string& gameLabel(const Game& game) {
  return showFilenames ? game.filename : game.title;
}

std::string titleFromFilename(const std::string& filename) {
  const std::string stem = filename.substr(0, filename.find_last_of('.'));
  std::string key = stem;
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) { return std::tolower(ch); });
  const size_t releaseSuffix = key.find("-r");
  if (releaseSuffix != std::string::npos && releaseSuffix + 2 < key.size() &&
      std::isdigit(static_cast<unsigned char>(key[releaseSuffix + 2]))) {
    key.resize(releaseSuffix);
  }

  static constexpr struct { const char* stem; const char* title; } known[] = {
    {"905", "9:05"},
    {"advent", "Colossal Cave Adventure"},
    {"hhgg", "The Hitchhiker's Guide to the Galaxy"},
    {"hitchhiker", "The Hitchhiker's Guide to the Galaxy"},
    {"hitchhiker-invclues", "The Hitchhiker's Guide to the Galaxy"},
    {"dreamhold", "The Dreamhold"},
    {"minizork", "Mini-Zork I"},
    {"suite", "The Z-Machine Test Suite"},
    {"suvehnux", "Suveh Nux"},
    {"tangle", "Spider and Web"},
    {"zork1", "Zork I"},
    {"zork2", "Zork II"},
    {"zork3", "Zork III"},
  };
  for (const auto& entry : known) {
    if (key == entry.stem) {
      return entry.title;
    }
  }

  std::string title;
  bool newWord = true;
  for (size_t i = 0; i < stem.size(); ++i) {
    const unsigned char ch = stem[i];
    if (ch == '_' || ch == '-' || std::isspace(ch)) {
      newWord = true;
      continue;
    }
    if (!title.empty() && (newWord ||
        (std::isupper(ch) && i > 0 && std::islower(static_cast<unsigned char>(stem[i - 1]))))) {
      title += ' ';
    }
    title += static_cast<char>(newWord ? std::toupper(ch) : ch);
    newWord = false;
  }
  return title.empty() ? filename : title;
}

std::string readGameTitle(const std::string& path, const std::string& filename, bool blorb) {
  char title[128] = {};
  FILE* overrideFile = fopen((path + ".title").c_str(), "r");
  if (overrideFile) {
    const bool gotTitle = fgets(title, sizeof(title), overrideFile) != nullptr;
    fclose(overrideFile);
    if (gotTitle) {
      std::string text(title);
      const size_t first = text.find_first_not_of(" \t\r\n");
      if (first != std::string::npos) {
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
      }
    }
  }
  if (blorb && terminal_read_title(path.c_str(), title, sizeof(title))) {
    return title;
  }
  return titleFromFilename(filename);
}

void readPreferences() {
  preferencesReady = preferences.begin("adventure", false);
  if (preferencesReady) {
    const uint8_t storedPalette = preferences.getUChar("palette", defaultPalette);
    activePalette = storedPalette < paletteCount ? storedPalette : defaultPalette;
    showFilenames = preferences.getBool("filenames", false);
  }
}

void keepSelectionVisible() {
  if (selectedGame < firstGame) {
    firstGame = selectedGame;
  } else if (selectedGame >= firstGame + menuRows) {
    firstGame = selectedGame - menuRows + 1;
  }
}

void sortGames(const std::string& selectedFilename) {
  std::sort(games.begin(), games.end(), [](const Game& left, const Game& right) {
    const int labelOrder = strcasecmp(gameLabel(left).c_str(), gameLabel(right).c_str());
    if (labelOrder != 0) {
      return labelOrder < 0;
    }
    const int filenameOrder = strcasecmp(left.filename.c_str(), right.filename.c_str());
    return filenameOrder == 0 ? left.filename < right.filename : filenameOrder < 0;
  });
  const auto selected = std::find_if(games.begin(), games.end(), [&](const Game& game) {
    return game.filename == selectedFilename;
  });
  if (selected != games.end()) {
    selectedGame = static_cast<size_t>(selected - games.begin());
    keepSelectionVisible();
  }
}

void scanGames() {
  const std::string previous = games.empty() ? "" : games[selectedGame].filename;
  games.clear();
  selectedGame = firstGame = 0;
  menuMessage.clear();
  menuDirty = menuLayoutDirty = true;

  // Remount on refresh so the menu can recover from a missing or replaced card.
  if (cardMounted) {
    SD.end();
  }
  cardMounted = SD.begin(sdSelect, SPI, 4000000, "/sd");
  if (!cardMounted) {
    menuMessage = "Insert a FAT32 microSD card.";
    Serial.println(menuMessage.c_str());
    return;
  }

  File root = SD.open("/");
  if (!root || !root.isDirectory()) {
    menuMessage = "Could not read the microSD card.";
    Serial.println(menuMessage.c_str());
    return;
  }

  for (File file = root.openNextFile(); file; file = root.openNextFile()) {
    if (!file.isDirectory() && isStoryName(file.name())) {
      const std::string filename = file.name();
      const std::string path = "/sd/" + filename;
      FrotzStory story;
      const char* error;
      const bool valid = terminal_probe_story(path.c_str(), &story, &error);
      games.push_back({filename, readGameTitle(path, filename, valid && story.offset != 0),
                       valid && terminal_has_save(path.c_str(), &story)});
    }
  }

  sortGames(previous);
  if (games.empty()) {
    menuMessage = "Copy .z3, .z5 or .zblorb stories to the card.";
  }
  Serial.printf("GAME MENU: %u stories\n", static_cast<unsigned>(games.size()));
}

void showMenu(Screen next, const char* message = "") {
  inputMode = Input::None;
  screen = next;
  menuMessage = message;
  menuDirty = menuLayoutDirty = true;
}

void showGames(const char* message = "") {
  showMenu(Screen::Games, message);
}

void printClipped(const char* text, size_t maxChars) {
  const size_t length = strlen(text);
  if (length > maxChars) {
    display.printf("%.*s...", static_cast<int>(maxChars - 3), text);
  } else {
    display.print(text);
  }
}

size_t menuLabelColumns(size_t rowColumns, size_t badgeLength) {
  return rowColumns - 2 - (badgeLength ? (badgeLength + 3) / 2 : 0);
}

void drawMenuRow(size_t row, const char* text, bool selected, const char* badge = "") {
  const size_t rowColumns = screen == Screen::Games ? storyColumns : columns;
  const int y = menuTop + row * menuRowHeight;
  const Palette& colors = palette();
  const uint16_t selectionColor = appleTheme() ? appleColors[AppleDarkBlue] : colors.foreground;
  const uint16_t background = selected ? selectionColor : colors.background;
  const uint16_t foreground = selected && !appleTheme() ? colors.background : colors.foreground;
  display.fillRect(margin, y, rowColumns * cellWidth, menuRowHeight, background);
  display.setTextColor(foreground, background);
  display.setCursor(margin, y + 2);
  if (selected) {
    display.setTextColor(appleTheme() ? appleColors[AppleOrange] : foreground, background);
    display.print('>');
    display.setTextColor(foreground, background);
  }
  display.setCursor(margin + cellWidth + 2, y + 2);

  const size_t badgeLength = strlen(badge);
  printClipped(text, menuLabelColumns(rowColumns, badgeLength));
  if (badgeLength) {
    display.setTextSize(1);
    const uint16_t badgeColor = appleTheme() ? appleColors[AppleYellow] : selected ? foreground : colors.accent;
    display.setTextColor(badgeColor, background);
    display.setCursor(margin + rowColumns * cellWidth - (badgeLength + 2) * 6, y + 6);
    display.print(badge);
    display.setTextSize(2);
  }
}

void drawMenuDragon() {
  constexpr int scale = 5;
  constexpr int spriteWidth = sizeof(menuDragon[0]) - 1;
  constexpr int spriteHeight = sizeof(menuDragon) / sizeof(menuDragon[0]);
  constexpr int x = margin + storyColumns * cellWidth + 10;
  constexpr int y = menuTop + menuRows * menuRowHeight - spriteHeight * scale;
  const Palette& colors = palette();
  // Average RGB565 channels separately so the dragon follows the active theme.
  constexpr uint16_t mask = 0xF7DE;
  const uint16_t midtone = ((colors.accent & mask) >> 1) + ((colors.foreground & mask) >> 1);
  const uint16_t shades[] = {
    colors.background,
    appleTheme() ? appleColors[ApplePurple] : colors.accent,
    appleTheme() ? appleColors[AppleMediumBlue] : midtone,
    appleTheme() ? appleColors[AppleLightGreen] : colors.foreground,
  };

  for (int row = 0; row < spriteHeight; ++row) {
    const char* pixels = menuDragon[row];
    for (int start = 0, end = 0; start < spriteWidth; start = end) {
      const char shade = pixels[start];
      for (end = start + 1; end < spriteWidth && pixels[end] == shade; ++end) {
      }
      if (shade != '.') {
        display.fillRect(x + start * scale, y + row * scale, (end - start) * scale, scale,
                         shades[shade - '0']);
      }
    }
  }
}

void drawAbout() {
  display.setTextColor(palette().foreground, palette().background);
  display.setTextSize(2);
  display.setCursor(margin, 70);
  display.print("Adventure Game! by bpwagner");
  display.setCursor(margin, 106);
  display.print("Freenove FNK0103 ESP32 board");
  display.setCursor(margin, 128);
  display.print("M5Stack Unit CardKB v1.1 keyboard");
  display.setCursor(margin, 162);
  display.print("Frotz Z-machine interpreter");
  display.setCursor(margin, 184);
  display.print("(c) 1995-1997 Stefan Jokisch");
  display.setCursor(margin, 206);
  display.print("Unix port: Galen Hazelwood");
  display.setCursor(margin, 228);
  display.print("Maintainer: David Griffith");
  display.setCursor(margin, 250);
  display.print("GNU GPL v2 or later");
}

void drawMenu() {
  if (!menuDirty) {
    return;
  }
  menuDirty = false;
  const Palette& colors = palette();
  const bool choosingStory = screen == Screen::Games;
  const bool choosingAction = screen == Screen::SavedGame;
  const bool configuring = screen == Screen::Settings;
  const size_t activeSelection = configuring ? selectedPalette : choosingAction ? selectedAction : selectedGame;
  const size_t firstRow = choosingStory ? firstGame : 0;
  static size_t drawnSelection = 0;
  static size_t drawnFirstRow = 0;
  const bool redrawRows = menuLayoutDirty || firstRow != drawnFirstRow;
  const auto rowChanged = [&](size_t index) {
    return redrawRows || (drawnSelection != activeSelection &&
                           (index == activeSelection || index == drawnSelection));
  };

  if (menuLayoutDirty) {
    display.fillScreen(colors.background);
    display.setTextColor(appleTheme() ? appleColors[AppleOrange] : colors.foreground, colors.background);
    display.setTextSize(3);
    display.setCursor(margin, 16);
    switch (screen) {
      case Screen::Settings: display.print("Settings"); break;
      case Screen::About: display.print("About"); break;
      case Screen::SavedGame: display.print("Play Game"); break;
      default: display.print("Adventure Game!"); break;
    }
    const int headerWidth = (choosingStory ? storyColumns : columns) * cellWidth;
    display.drawFastHLine(margin, 50, headerWidth,
                          appleTheme() ? appleColors[ApplePurple] : colors.foreground);
    if (!appleTheme()) {
      display.drawFastHLine(margin, 53, headerWidth, colors.accent);
    }
    display.drawFastHLine(margin, 272, columns * cellWidth, colors.accent);
    if (choosingStory) {
      drawMenuDragon();
    }
    menuLayoutDirty = false;
  }

  display.setTextSize(2);
  display.setTextColor(colors.foreground, colors.background);
  display.setCursor(margin, 63);
  if (screen == Screen::About) {
    drawAbout();
  } else if (configuring) {
    display.print("Color theme");
    for (size_t row = 0; row < paletteCount; ++row) {
      if (rowChanged(row)) {
        drawMenuRow(row, palettes[row].name, row == selectedPalette, row == activePalette ? "ACTIVE" : "");
      }
    }
  } else if (choosingAction) {
    printClipped(gameLabel(games[selectedGame]).c_str(), columns);
    for (size_t row = 0; row < sizeof(savedActions) / sizeof(savedActions[0]); ++row) {
      if (rowChanged(row)) {
        drawMenuRow(row, savedActions[row], row == selectedAction);
      }
    }
  } else if (games.empty()) {
    if (redrawRows) {
      drawMenuRow(0, "Refresh card", true);
    }
  } else {
    display.setTextSize(1);
    display.setTextColor(colors.accent, colors.background);
    display.print(showFilenames ? "STORY FILES" : "STORY TITLES");
    display.setTextSize(2);
    for (size_t row = 0; row < menuRows && firstGame + row < games.size(); ++row) {
      const size_t index = firstGame + row;
      if (rowChanged(index)) {
        const Game& game = games[index];
        drawMenuRow(row, gameLabel(game).c_str(), index == selectedGame, game.saved ? "SAVED" : "");
      }
    }
  }
  drawnSelection = activeSelection;
  drawnFirstRow = firstRow;

  display.fillRect(margin, 279, columns * cellWidth, 37, colors.background);
  display.setTextSize(1);
  display.setTextColor(colors.foreground, colors.background);
  display.setCursor(margin, 280);
  if (!menuMessage.empty()) {
    printClipped(menuMessage.c_str(), columns * 2);
  } else if (choosingStory && !games.empty()) {
    const Game& game = games[selectedGame];
    const std::string& label = gameLabel(game);
    if (label.size() > menuLabelColumns(storyColumns, game.saved ? sizeof("SAVED") - 1 : 0)) {
      printClipped(label.c_str(), columns * 2);
    }
  }
  display.setCursor(margin, 296);
  if (appleTheme()) {
    display.setTextColor(colors.accent, colors.background);
  }
  if (!keyboardPresent) {
    display.print("CONNECT CARDKB");
  } else if (screen == Screen::About) {
    display.print("ESC or ENTER Back");
  } else if (configuring) {
    display.print("UP/DOWN Select   ENTER Apply   ESC Back");
  } else if (choosingAction) {
    display.print("UP/DOWN Select   ENTER Confirm   ESC Back");
  } else {
    display.printf("%s   F %s",
                   games.empty() ? "ENTER Refresh" : "UP/DOWN Select   ENTER Open",
                   showFilenames ? "Titles" : "Filenames");
    display.setCursor(margin, 308);
    display.print("S Settings   A About   R Refresh");
  }
  display.setTextSize(2);
}

void beginPlay(bool resume) {
  clearTranscript();
  for (auto& row : upperWindow) {
    memset(row, ' ', columns);
  }
  commandLength = 0;
  command[0] = '\0';
  inputMode = Input::None;
  resumeFrotz = resume;

  screen = Screen::Story;
  transcriptDirty = promptDirty = true;
  display.fillScreen(palette().background);
  display.setTextSize(1);
  display.setTextColor(palette().accent, palette().background);
  display.setCursor(margin, 312);
  display.print("ESC Game menu   UP/DOWN Scroll   Type SAVE to keep progress");
  display.setTextSize(2);
  display.setTextColor(palette().foreground, palette().background);
  Serial.printf("Playing %s%s\n", games[selectedGame].filename.c_str(), resume ? " (continued)" : "");
}

void openSelectedGame() {
  Game& game = games[selectedGame];
  activePath = "/sd/" + game.filename;
  const char* error = nullptr;
  if (game.filename.size() > 247) {
    showGames("Story filename is too long.");
    return;
  }
  if (!terminal_probe_story(activePath.c_str(), &frotzStory, &error)) {
    showGames(error);
    return;
  }
  game.saved = terminal_has_save(activePath.c_str(), &frotzStory);
  if (game.saved) {
    selectedAction = 0;
    showMenu(Screen::SavedGame);
  } else {
    beginPlay(false);
  }
}

void moveSelection(size_t& selection, size_t count, uint8_t key) {
  if (key == keyUp && selection > 0) {
    --selection;
  } else if (key == keyDown && selection + 1 < count) {
    ++selection;
  }
}

void handleMenuKey(uint8_t key) {
  const bool enter = key == '\r' || key == '\n';
  if (screen == Screen::About) {
    if (key == keyEscape || enter) {
      showGames();
    }
    return;
  }
  if (screen == Screen::Settings) {
    if (key == keyEscape) {
      showGames();
    } else if (enter) {
      if (activePalette != selectedPalette) {
        activePalette = selectedPalette;
        if (preferencesReady) {
          preferences.putUChar("palette", activePalette);
        }
        menuDirty = menuLayoutDirty = true;
      }
    } else if (key == keyUp || key == keyDown) {
      moveSelection(selectedPalette, paletteCount, key);
      menuDirty = true;
    }
    return;
  }
  if (screen == Screen::SavedGame) {
    if (key == keyEscape || (enter && selectedAction == 2)) {
      showGames();
    } else if (enter) {
      beginPlay(selectedAction == 0);
    } else if (key == keyUp || key == keyDown) {
      moveSelection(selectedAction, sizeof(savedActions) / sizeof(savedActions[0]), key);
      menuDirty = true;
    }
  } else if (key == 'f' || key == 'F') {
    const std::string selectedFilename = games.empty() ? "" : games[selectedGame].filename;
    showFilenames = !showFilenames;
    sortGames(selectedFilename);
    if (preferencesReady) {
      preferences.putBool("filenames", showFilenames);
    }
    menuDirty = menuLayoutDirty = true;
  } else if (key == 's' || key == 'S') {
    selectedPalette = activePalette;
    showMenu(Screen::Settings);
  } else if (key == 'a' || key == 'A') {
    showMenu(Screen::About);
  } else if (key == 'r' || key == 'R' || (enter && games.empty())) {
    scanGames();
  } else if (enter) {
    openSelectedGame();
  } else if (key == keyUp || key == keyDown) {
    moveSelection(selectedGame, games.size(), key);
    keepSelectionVisible();
    menuDirty = true;
  }
}

void stripStoryPrompt() {
  // Put the game's plain > prompt in the editing row.
  if (strcmp(history[currentRow], ">") == 0 || strcmp(history[currentRow], "> ") == 0) {
    history[currentRow][0] = '\0';
    currentColumn = 0;
  }
}

void echoCommand() {
  if (currentColumn) {
    appendText("\n", 1);
  }
  appendText("> ", 2);
  appendText(command, commandLength);
  Serial.printf("Command: %s\n", command);
}

void scrollTranscript(uint8_t key) {
  const size_t rows = visibleRows - upperHeight();
  if (key == keyUp && lineCount > rows) {
    scrollOffset = min(scrollOffset + 1, lineCount - rows);
  } else if (key == keyDown && scrollOffset) {
    --scrollOffset;
  }
  transcriptDirty = promptDirty = true;
}

void editCommand(uint8_t key, size_t limit) {
  if (scrollOffset) {
    scrollOffset = 0;
    transcriptDirty = true;
  }
  if ((key == '\b' || key == 0x7F) && commandLength > 0) {
    command[--commandLength] = '\0';
  } else if (key >= ' ' && key <= '~' && commandLength < limit) {
    command[commandLength++] = static_cast<char>(key);
    command[commandLength] = '\0';
  }
  promptDirty = true;
}

int readBoardKey() {
  const bool received = Wire.requestFrom(keyboardAddress, size_t{1}) == 1;
  if (!keyboardChecked || received != keyboardPresent) {
    keyboardChecked = true;
    keyboardPresent = received;
    promptDirty = menuDirty = true;
    Serial.println(received ? "CARDKB READY (0x5F)" : "CARDKB NOT FOUND");
  }
  return received ? max(Wire.read(), 0) : 0;
}

void drawFrotzUpperChar(int row, int column, char value) {
  if (row >= 1 && row <= static_cast<int>(upperRows) &&
      column >= 1 && column <= static_cast<int>(columns)) {
    upperWindow[row - 1][column - 1] = value;
    transcriptDirty = true;
  }
}

void eraseFrotzWindow(int top, int left, int bottom, int right, int window) {
  if (window == -2 || window == 0) {
    clearTranscript();
  }
  if (window == -2 || window == 1 || window == 7) {
    if (window == -2) {
      top = left = 1;
      bottom = upperRows;
      right = columns;
    }
    const int start = max(left, 1);
    const int end = min(right, static_cast<int>(columns));
    for (int row = max(1, top); row <= min(bottom, static_cast<int>(upperRows)); ++row) {
      if (end >= start) {
        memset(upperWindow[row - 1] + start - 1, ' ', end - start + 1);
      }
    }
    transcriptDirty = true;
  }
}

int translateSingleKey(int key) {
  switch (key) {
    case keyUp: return 0x81;
    case keyDown: return 0x82;
    case keyLeft: return 0x83;
    case keyRight: return 0x84;
    case '\r': case '\n': return '\r';
    case '\b': case 0x7F: return '\b';
    default: return key >= ' ' && key <= '~' ? key : 0;
  }
}

int readFrotzInput(char* line, size_t limit, unsigned timeoutMs) {
  const size_t capacity = min(limit, sizeof(command) - 1);
  commandLength = line ? strnlen(line, capacity) : 0;
  if (line) {
    memcpy(command, line, commandLength);
    stripStoryPrompt();
  }
  command[commandLength] = '\0';
  scrollOffset = 0;
  inputMode = line ? Input::Command : Input::SingleKey;
  transcriptDirty = promptDirty = true;
  drawTerminal();

  const uint32_t start = millis();
  int result = 0;
  while (!timeoutMs || static_cast<uint32_t>(millis() - start) < timeoutMs) {
    const int key = readBoardKey();
    if (key == keyEscape) {
      result = -1;
      break;
    }
    if (!line) {
      result = translateSingleKey(key);
      if (result) {
        break;
      }
    } else if (key == '\r' || key == '\n') {
      echoCommand();
      result = '\r';
      break;
    } else if (key == keyUp || key == keyDown) {
      scrollTranscript(key);
    } else if (key) {
      editCommand(key, capacity);
    }
    drawTerminal();
    delay(15);
  }

  if (line && result >= 0) {
    memcpy(line, command, commandLength + 1);
  }
  inputMode = Input::None;
  promptDirty = true;
  return result;
}

void runFrotz() {
  static const FrotzTerminal terminal = {
    output,
    drawFrotzUpperChar,
    eraseFrotzWindow,
    readFrotzInput,
    [](unsigned timeout) { return readFrotzInput(nullptr, 0, timeout); },
    [] { delay(1); },
  };
  const FrotzResult result = terminal_run_story(activePath.c_str(), &frotzStory, &terminal,
                                               esp_random(), resumeFrotz);
  games[selectedGame].saved = terminal_has_save(activePath.c_str(), &frotzStory);
  showGames(result == FROTZ_ERROR ? terminal_story_error() :
            result == FROTZ_FINISHED ? "Story ended." : "");
}
}

void setup() {
  Serial.begin(115200);
  readPreferences();
  display.init();
  display.setRotation(1);
  display.fillScreen(palette().background);
  display.setTextFont(1);
  display.setTextSize(2);
  display.setTextWrap(false);
  display.setTextColor(palette().foreground, palette().background);

  Wire.begin(keyboardSda, keyboardScl, 100000);
  SPI.begin(sdSck, sdMiso, sdMosi, sdSelect);

  scanGames();
  readBoardKey();
  drawMenu();
}

void loop() {
  const int key = readBoardKey();
  if (key) {
    handleMenuKey(key);
  }
  if (screen == Screen::Story) {
    runFrotz();
  }
  drawMenu();
  delay(20);
}
