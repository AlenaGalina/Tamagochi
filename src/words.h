// German words: ten new cards a day plus spaced-repetition reviews (Leitner boxes).
#pragma once
#include "deck.h"

#define WORDS_PER_DAY 10

void wordsBegin();
void wordsSetDay(int day);    // local day number; a new day brings ten new words
int wordsLeftToday();         // reviews due plus new words still to meet today
int wordsNewToday();          // new words already met today
int wordsNext();              // card to show next, -1 when today's work is done
bool wordsIsNew(int card);    // never seen before
void wordsAnswer(int card, bool known);
