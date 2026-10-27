
#pragma once
#include <Arduino.h>

#ifndef NEWS_MAX
#define NEWS_MAX 20
#endif

// Increase title and add description
#ifndef NEWS_TITLE_LEN
#define NEWS_TITLE_LEN 160
#endif
#ifndef NEWS_DESC_LEN
#define NEWS_DESC_LEN 600
#endif

struct NewsData {
  int count;
  int index;
  char title[NEWS_MAX][NEWS_TITLE_LEN];
  char desc[NEWS_MAX][NEWS_DESC_LEN];
  char updated[20];
  int channel;   // 0: GLOBAL, 1: BBC
  int category;  // 0: TOP, 1: TECH, 2: BIZ, 3: SPORTS, 4: ENTERTAIN, 5: WORLD, 6: SCI, 7: HEALTH
};

void newsBegin();
void newsLoop();

bool newsIsBusy();
void newsRequestFetch();
void newsSetChannel(int c);
void newsSetCategory(int c);
bool newsConsumeResult(bool& ok, NewsData& out);

const NewsData& newsGetCached();

void newsNext();
void newsPrev();
