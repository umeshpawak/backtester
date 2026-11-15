// Moving-Average Crossover Backtester (single file, no external libraries)
//
// Build: g++ -std=c++17 -O2 backtest.cpp -o backtest
// Run:   ./backtest prices.csv [shortWindow] [longWindow]
// CSV:   a header row, then one row per day. Works with simple "date,close"
// files and with
//        Yahoo Finance downloads (quoted dates like "Nov 13, 2026",
//        newest-first order). The column named "Close" is used; otherwise the
//        second column.
//
// Strategy: BUY when the short moving average crosses ABOVE the long one,
//           SELL when it crosses BELOW. Long-only, all-in, one position at a
//           time.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// Money is stored as integer cents, never double, to avoid rounding errors.
using Money = long long;

const Money START_CASH = 10'000'000; // 100,000.00
const int FEE_BPS = 10; // fee per trade in basis points (10 = 0.10%)

struct Bar {
  std::string date;
  Money close; // closing price in cents
};

// Cents -> "1234.56" for printing.
std::string fmt(Money cents) {
  std::ostringstream os;
  if (cents < 0) {
    os << '-';
    cents = -cents;
  }
  os << cents / 100 << '.' << std::setw(2) << std::setfill('0') << cents % 100;
  return os.str();
}

// Split one CSV line into fields; a "quoted, field" keeps its comma.
std::vector<std::string> splitCsv(const std::string &line) {
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false;
  for (char c : line) {
    if (c == '"')
      quoted = !quoted;
    else if (c == ',' && !quoted) {
      out.push_back(cur);
      cur.clear();
    } else if (c != '\r')
      cur += c;
  }
  out.push_back(cur);
  return out;
}

// "Nov 13, 2026" -> "2026-11-13" so dates sort correctly. Other formats pass
// through unchanged.
std::string isoDate(const std::string &d) {
  static const std::string months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4];
  int day, year;
  if (std::sscanf(d.c_str(), "%3s %d, %d", mon, &day, &year) != 3)
    return d;
  size_t m = months.find(mon);
  if (m == std::string::npos || m % 3 != 0)
    return d;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", year,
                static_cast<int>(m / 3) + 1, day);
  return buf;
}

// Load prices, using the "Close" column, and return them oldest-first.
// Bad rows are skipped instead of crashing.
std::vector<Bar> loadPrices(const std::string &path) {
  std::ifstream in(path);
  if (!in)
    throw std::runtime_error("cannot open " + path);
  std::vector<Bar> bars;
  std::string line;
  std::getline(in, line); // header row
  std::vector<std::string> header = splitCsv(line);
  size_t col = 1; // default: second column
  for (size_t i = 0; i < header.size(); ++i)
    if (header[i] == "Close" || header[i] == "close")
      col = i;

  while (std::getline(in, line)) {
    std::vector<std::string> f = splitCsv(line);
    if (f.size() <= col)
      continue;
    std::string &p = f[col];
    p.erase(std::remove(p.begin(), p.end(), ','),
            p.end()); // "1,234.56" -> "1234.56"
    try {
      bars.push_back({isoDate(f[0]), std::llround(std::stod(p) * 100)});
    } catch (...) { /* ignore malformed price */
    }
  }
  // Yahoo gives newest first; sorting by ISO date puts oldest first for any
  // input order.
  std::sort(bars.begin(), bars.end(),
            [](const Bar &a, const Bar &b) { return a.date < b.date; });
  return bars;
}

// Simple moving average of the n closes ending at index `end` (inclusive).
// It's a statistic, not money, so double is fine here.
double sma(const std::vector<Bar> &b, size_t end, int n) {
  Money sum = 0;
  for (size_t i = end + 1 - n; i <= end; ++i)
    sum += b[i].close;
  return static_cast<double>(sum) / n;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0]
              << " prices.csv [shortWindow=5] [longWindow=20]\n";
    return 1;
  }
  try {
    int shortW = argc > 2 ? std::stoi(argv[2]) : 5;
    int longW = argc > 3 ? std::stoi(argv[3]) : 20;
    if (shortW <= 0 || shortW >= longW) {
      std::cerr << "Need 0 < shortWindow < longWindow\n";
      return 1;
    }
    std::vector<Bar> bars = loadPrices(argv[1]);
    if (bars.size() <= static_cast<size_t>(longW)) {
      std::cerr << "Not enough data for a " << longW << "-day average\n";
      return 1;
    }

    Money cash = START_CASH, entryCost = 0;
    long long shares = 0;
    int trades = 0, wins = 0;

    auto buy = [&](const Bar &bar) {
      // Largest whole number of shares we can afford including the fee.
      shares = cash * 10000 / (bar.close * (10000 + FEE_BPS));
      if (shares == 0)
        return;
      Money cost = shares * bar.close;
      entryCost = cost + cost * FEE_BPS / 10000;
      cash -= entryCost;
      std::cout << bar.date << "  BUY  " << shares << " @ " << fmt(bar.close)
                << '\n';
    };
    auto sell = [&](const Bar &bar) {
      Money proceeds = shares * bar.close;
      proceeds -= proceeds * FEE_BPS / 10000;
      cash += proceeds;
      ++trades;
      if (proceeds > entryCost)
        ++wins; // a "win" = round trip made money after fees
      std::cout << bar.date << "  SELL " << shares << " @ " << fmt(bar.close)
                << '\n';
      shares = 0;
    };

    // Start where a full long window exists for both yesterday (i-1) and today
    // (i).
    for (size_t i = longW; i < bars.size(); ++i) {
      double prevShort = sma(bars, i - 1, shortW),
             prevLong = sma(bars, i - 1, longW);
      double curShort = sma(bars, i, shortW), curLong = sma(bars, i, longW);

      if (shares == 0 && prevShort <= prevLong && curShort > curLong)
        buy(bars[i]);
      else if (shares > 0 && prevShort >= prevLong && curShort < curLong)
        sell(bars[i]);
    }
    if (shares > 0)
      sell(bars.back()); // close any open position at the last price

    // Benchmark: buy on the first tradable day, hold to the end (no fees, for
    // simplicity).
    Bar first = bars[longW], last = bars.back();
    long long bhShares = START_CASH / first.close;
    Money bhFinal = START_CASH - bhShares * first.close + bhShares * last.close;

    Money profit = cash - START_CASH;
    std::cout << "\n--- Results (" << shortW << "/" << longW << " SMA, fee "
              << FEE_BPS << " bps) ---\n"
              << "Final cash:     " << fmt(cash) << '\n'
              << "Total profit:   " << fmt(profit) << "  (" << std::fixed
              << std::setprecision(2) << 100.0 * profit / START_CASH << "%)\n"
              << "Round trips:    " << trades << '\n'
              << "Win rate:       " << (trades ? 100.0 * wins / trades : 0.0)
              << "%\n"
              << "Buy & hold:     " << fmt(bhFinal - START_CASH) << "  ("
              << 100.0 * (bhFinal - START_CASH) / START_CASH << "%)\n";
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << '\n';
    return 1;
  }
}