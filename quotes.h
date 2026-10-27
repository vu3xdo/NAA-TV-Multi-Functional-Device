
#pragma once
#include <Arduino.h>

struct QuoteData {
  char quote[256];
  char author[64];
  char updated[20];
};

void quotesBegin();
void quotesLoop();

bool quotesIsBusy();
void quotesRequestFetch();
bool quotesConsumeResult(bool& ok, QuoteData& out);

const QuoteData& quotesGetCached();
void quotesNext(); // NEW: Fetch next quote

