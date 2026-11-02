// Starter German deck. Umlauts are CP437 codes for the built-in font:
// \x84 ä, \x94 ö, \x81 ü, \x8E Ä, \x99 Ö, \x9A Ü, \xE1 ß.
#pragma once

struct Card {
  const char *de;
  const char *en;
};

static const Card CARDS[] = {
    {"der Hund", "dog"},
    {"die Katze", "cat"},
    {"das Fr\x81hst\x81" "ck", "breakfast"},
    {"laufen", "to run"},
    {"die Arbeit", "work"},
    {"m\x81" "de", "tired"},
    {"gl\x81" "cklich", "happy"},
    {"die Stra\xE1" "e", "street"},
    {"sch\x94n", "beautiful"},
    {"das M\x84" "dchen", "girl"},
    {"lernen", "to learn"},
    {"die Uhr", "clock"},
    {"der Schl\x81ssel", "key"},
    {"heute", "today"},
    {"morgen", "tomorrow"},
    {"vielleicht", "maybe"},
    {"die T\x81r", "door"},
    {"\x9A" "ben", "to practise"},
    {"der Apfel", "apple"},
    {"schnell", "fast"},
    {"langsam", "slow"},
    {"die Wolke", "cloud"},
    {"der Wald", "forest"},
    {"neugierig", "curious"},
    {"das Gl\x81" "ck", "luck"},
    {"tr\x84umen", "to dream"},
    {"wichtig", "important"},
    {"die Ruhe", "calm"},
    {"der B\x84r", "bear"},
    {"\x94" "ffnen", "to open"},
};
static const int CARD_COUNT = sizeof(CARDS) / sizeof(CARDS[0]);
