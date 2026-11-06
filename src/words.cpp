#include "words.h"
#include <Preferences.h>
#include "pet.h"

// Days until a card comes back, by Leitner box after a correct answer.
static const uint8_t INTERVAL[] = {0, 1, 3, 7, 14, 30};
#define TOP_BOX 5

static Preferences prefs;
static uint8_t box[DECK_SIZE];   // 0 = never seen
static uint16_t due[DECK_SIZE];  // local day the card is due again
static uint16_t nextNew = 0;     // first card never introduced
static int today = 0;
static uint8_t newToday = 0;
static bool rewarded = false;    // today's finishing bonus already given
static int cursor = -1;          // last card shown, so a missed card doesn't repeat at once

static void save() {
  uint8_t blob[DECK_SIZE * 3];
  for (int i = 0; i < DECK_SIZE; i++) {
    blob[i * 3] = box[i];
    blob[i * 3 + 1] = due[i] & 0xFF;
    blob[i * 3 + 2] = due[i] >> 8;
  }
  prefs.putBytes("prog", blob, sizeof(blob));
  prefs.putUShort("next", nextNew);
  prefs.putInt("day", today);
  prefs.putUChar("new", newToday);
  prefs.putBool("rewarded", rewarded);
}

void wordsBegin() {
  prefs.begin("words", false);
  static uint8_t blob[DECK_SIZE * 3];
  size_t got = prefs.getBytes("prog", blob, sizeof(blob));  // a grown deck keeps old progress
  for (int i = 0; i < DECK_SIZE; i++) {
    bool have = (size_t)(i * 3 + 2) < got;
    box[i] = have ? blob[i * 3] : 0;
    due[i] = have ? blob[i * 3 + 1] | blob[i * 3 + 2] << 8 : 0;
  }
  nextNew = prefs.getUShort("next", 0);
  today = prefs.getInt("day", 0);
  newToday = prefs.getUChar("new", 0);
  rewarded = prefs.getBool("rewarded", false);
}

void wordsSetDay(int day) {
  if (day == today) return;
  today = day;
  newToday = 0;
  rewarded = false;
  save();
}

static bool isDue(int i) { return box[i] > 0 && due[i] <= today; }

int wordsLeftToday() {
  int left = 0;
  for (int i = 0; i < DECK_SIZE; i++) left += isDue(i);
  int fresh = WORDS_PER_DAY - newToday;
  if (fresh > DECK_SIZE - nextNew) fresh = DECK_SIZE - nextNew;
  return left + (fresh > 0 ? fresh : 0);
}

int wordsNewToday() { return newToday; }

int wordsNext() {
  // Due reviews first, starting after the last card shown, then new words; the
  // card just missed comes back only when nothing else is left.
  for (int k = 0; k < DECK_SIZE; k++) {
    int i = (cursor + 1 + k) % DECK_SIZE;
    if (i != cursor && isDue(i)) return i;
  }
  if (newToday < WORDS_PER_DAY && nextNew < DECK_SIZE) return nextNew;
  if (cursor >= 0 && isDue(cursor)) return cursor;
  return -1;
}

bool wordsIsNew(int card) { return box[card] == 0; }

void wordsAnswer(int card, bool known) {
  if (card < 0 || card >= DECK_SIZE) return;
  if (box[card] == 0) {
    newToday++;
    if (card == nextNew) nextNew++;
  }
  if (known) {
    if (box[card] < TOP_BOX) box[card]++;
    due[card] = today + INTERVAL[box[card]];
  } else {
    box[card] = 1;
    due[card] = today;  // again later today
  }
  cursor = card;
  petCardAnswered(known);
  if (!rewarded && wordsLeftToday() == 0) {
    rewarded = true;
    petWordsDone();
  }
  save();
}
