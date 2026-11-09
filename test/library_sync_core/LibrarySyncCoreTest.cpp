#include <gtest/gtest.h>

#include "src/util/LibrarySyncCore.h"

using namespace LibrarySyncCore;

TEST(LibrarySyncCore, ParsesManifestLines) {
  ManifestLine line;
  EXPECT_FALSE(parseManifestLine("#manifest v1 2 1793800000", line));
  EXPECT_FALSE(parseManifestLine("", line));
  EXPECT_FALSE(parseManifestLine("only\tthree\tfields", line));
  EXPECT_FALSE(parseManifestLine("id\t1\t2\tabc\tpath\tnotabook.pdf", line));
  ASSERT_TRUE(parseManifestLine(
      "Adam Cesare - Clown\t2944438\t1760128440\tDEADBEEF00112233\tA-B/Adam%20Cesare/Clown.epub\tAdam Cesare - "
      "Clown.epub\r\n",
      line));
  EXPECT_EQ(line.id, "Adam Cesare - Clown");
  EXPECT_EQ(line.size, 2944438u);
  EXPECT_EQ(line.mtime, 1760128440u);
  EXPECT_EQ(line.path, "A-B/Adam%20Cesare/Clown.epub");
  EXPECT_EQ(line.filename, "Adam Cesare - Clown.epub");
  ASSERT_TRUE(parseManifestLine("x\t1\t2\t\tp/x.epub\tx.epub", line));  // hash is optional
  EXPECT_EQ(line.sha1, "");
}

TEST(LibrarySyncCore, NameKeyIgnoresOrderCaseAndPunctuation) {
  EXPECT_EQ(nameKey("Stephen King - 11-22-63.epub"), nameKey("11-22-63 - Stephen King.epub"));
  EXPECT_EQ(nameKey("The Caretaker - Marcus Kliewer"), nameKey("marcus kliewer - the caretaker.EPUB"));
  EXPECT_EQ(nameKey("Nocturnal: A Novel - Scott Sigler"), nameKey("Nocturnal_ A Novel - Scott Sigler.epub"));
  EXPECT_NE(nameKey("Infected - Scott Sigler"), nameKey("Contagious - Scott Sigler"));
  EXPECT_NE(nameKey("Dune"), nameKey("Dune Messiah"));
  EXPECT_EQ(nameKey("...---"), 0u);
}

TEST(LibrarySyncCore, DecisionTable) {
  ManifestLine line;
  line.sha1 = "aabbccdd11223344";
  const StateEntry same{1, 10, sha1Prefix32("aabbccdd")};
  const StateEntry older{1, 10, sha1Prefix32("00000001")};
  const StateEntry noHash{1, 10, 0};
  EXPECT_EQ(decide(line, nullptr, false, false), Decision::DOWNLOAD_NEW);
  EXPECT_EQ(decide(line, nullptr, true, false), Decision::ADOPT);
  EXPECT_EQ(decide(line, &same, true, true), Decision::UP_TO_DATE);
  EXPECT_EQ(decide(line, &same, false, true),
            Decision::DOWNLOAD_NEW);  // removed by hand: fetched again on a later pass
  EXPECT_EQ(decide(line, &older, false, false), Decision::DOWNLOAD_NEW);
  EXPECT_EQ(decide(line, &older, true, false), Decision::UP_TO_DATE);  // updates are opt-in
  EXPECT_EQ(decide(line, &older, true, true), Decision::DOWNLOAD_UPDATE);
  EXPECT_EQ(decide(line, &noHash, true, true), Decision::UP_TO_DATE);  // never refetch without a recorded hash
  line.sha1.clear();
  EXPECT_EQ(decide(line, &older, true, true), Decision::UP_TO_DATE);  // nor without a published hash
}

TEST(LibrarySyncCore, StateLinesRoundTrip) {
  const StateEntry e{fnv1a("Some Book"), 12345, sha1Prefix32("0123abcd")};
  StateEntry back;
  ASSERT_TRUE(parseStateLine(formatStateLine(e), back));
  EXPECT_EQ(back.idHash, e.idHash);
  EXPECT_EQ(back.size, e.size);
  EXPECT_EQ(back.sha1Prefix, e.sha1Prefix);
  EXPECT_FALSE(parseStateLine("garbage", back));
}

TEST(LibrarySyncCore, SchedulesAndBudgets) {
  EXPECT_FALSE(dueNow(Schedule::MANUAL, 100, 0, true, true));
  EXPECT_TRUE(dueNow(Schedule::EVERY_WAKE, 100, 99, false, false));
  EXPECT_FALSE(dueNow(Schedule::TWICE_DAILY, 900 + 719, 900, false, false));
  EXPECT_TRUE(dueNow(Schedule::TWICE_DAILY, 900 + 720, 900, false, false));  // 12 h after the last completed pass
  EXPECT_TRUE(dueNow(Schedule::TWICE_DAILY, 1000, 900, false, true));        // backlog keeps going
  EXPECT_TRUE(dueNow(Schedule::TWICE_DAILY, -1, 0, false, false));           // no clock: do not starve
  EXPECT_EQ(nextSchedule(Schedule::MANUAL), Schedule::TWICE_DAILY);          // turning it on picks twice daily
  EXPECT_EQ(nextSchedule(Schedule::TWICE_DAILY), Schedule::DAILY);
  EXPECT_EQ(nextSchedule(Schedule::CHARGING), Schedule::MANUAL);
  EXPECT_FALSE(dueNow(Schedule::DAILY, 1000, 900, false, false));
  EXPECT_TRUE(dueNow(Schedule::DAILY, 900 + 1440, 900, false, false));
  EXPECT_TRUE(dueNow(Schedule::DAILY, 1000, 900, false, true));  // backlog keeps going
  EXPECT_TRUE(dueNow(Schedule::DAILY, -1, 0, false, false));     // no clock: do not starve
  EXPECT_FALSE(dueNow(Schedule::CHARGING, 1000, 0, false, true));
  EXPECT_TRUE(dueNow(Schedule::CHARGING, 1000, 0, true, false));
  EXPECT_LT(budgetFor(false, false).maxBooks, budgetFor(false, true).maxBooks);
  EXPECT_LT(budgetFor(false, true).maxBooks, budgetFor(true, false).maxBooks);
}

TEST(LibrarySyncCore, WatchdogGivesUpOnNoStartAndOnStall) {
  // Nothing received: fine until 25 s, then NO_START (also across a millis() wrap).
  EXPECT_EQ(watchdog(1000, 0, 0, false), Watch::OK);
  EXPECT_EQ(watchdog(24999, 0, 0, false), Watch::OK);
  EXPECT_EQ(watchdog(25000, 0, 0, false), Watch::NO_START);
  EXPECT_EQ(watchdog(5000u, 0xFFFFF000u, 0, false, 25000, 30000), Watch::OK);  // 8.2 s since a wrapped start
  EXPECT_EQ(watchdog(30000u, 0xFFFFF000u, 0, false, 25000, 30000), Watch::NO_START);
  // Bytes flowed: only silence counts, and it takes 30 s.
  EXPECT_EQ(watchdog(100000, 0, 80000, true), Watch::OK);
  EXPECT_EQ(watchdog(110000, 0, 80000, true), Watch::STALLED);
  EXPECT_EQ(watchdog(110000, 0, 109999, true), Watch::OK);
}

TEST(LibrarySyncCore, FailuresBackOffAndQuarantineAfterThree) {
  FailEntry e;
  e.idHash = 0xABCDEF01;
  EXPECT_TRUE(shouldAttempt(&e, 1000, false));
  EXPECT_TRUE(shouldAttempt(nullptr, 1000, false));
  recordFailure(e, FailReason::NO_START, 1000, 500);
  EXPECT_EQ(e.count, 1);
  EXPECT_EQ(e.nextRetryMin, 1030);
  EXPECT_FALSE(shouldAttempt(&e, 1029, false));
  EXPECT_TRUE(shouldAttempt(&e, 1030, false));
  recordFailure(e, FailReason::STALLED, 1030, 500);
  EXPECT_EQ(e.nextRetryMin, 1030 + 120);
  EXPECT_FALSE(isQuarantined(e));
  recordFailure(e, FailReason::NOT_EPUB, 1200, 500);
  EXPECT_TRUE(isQuarantined(e));
  EXPECT_EQ(e.nextRetryMin, 1200 + 24 * 60);
  EXPECT_FALSE(shouldAttempt(&e, 1200 + 600, false));     // a bad book is left alone by background passes
  EXPECT_TRUE(shouldAttempt(&e, 1200 + 24 * 60, false));  // ...until the back-off is over
  EXPECT_TRUE(shouldAttempt(&e, 1200, true));             // 'Sync library now' retries it once
  recordFailure(e, FailReason::NOT_EPUB, 3000, 500);
  EXPECT_EQ(e.nextRetryMin, 3000 + 7 * 24 * 60);
  // No usable clock: books that are not quarantined are tried, quarantined ones wait.
  FailEntry fresh;
  recordFailure(fresh, FailReason::HTTP_ERROR, -1, 10);
  EXPECT_TRUE(shouldAttempt(&fresh, -1, false));
  EXPECT_FALSE(shouldAttempt(&e, -1, false));
}

TEST(LibrarySyncCore, FailLinesRoundTrip) {
  FailEntry e;
  e.idHash = 0x00C0FFEE;
  recordFailure(e, FailReason::TOO_BIG, 500, 118501344);
  FailEntry back;
  ASSERT_TRUE(parseFailLine(formatFailLine(e), back));
  EXPECT_EQ(back.idHash, e.idHash);
  EXPECT_EQ(back.count, e.count);
  EXPECT_EQ(back.nextRetryMin, e.nextRetryMin);
  EXPECT_EQ(back.reason, FailReason::TOO_BIG);
  EXPECT_EQ(back.size, 118501344u);
  EXPECT_FALSE(parseFailLine("garbage", back));
  EXPECT_FALSE(parseFailLine("00000000\t1\t2\t3\t4", back));  // id 0 is never a book
}

TEST(LibrarySyncCore, BigBooksWaitForALongBudget) {
  EXPECT_TRUE(fitsRemainingTime(3 * 1000 * 1000, 75 * 1000));         // 3 MB in 75 s: fine
  EXPECT_FALSE(fitsRemainingTime(119 * 1000 * 1000, 75 * 1000));      // Hidden Pictures on battery
  EXPECT_FALSE(fitsRemainingTime(119 * 1000 * 1000, 8 * 60 * 1000));  // ...and on the charger's 8 minutes
  EXPECT_TRUE(fitsRemainingTime(119 * 1000 * 1000, 20 * 60 * 1000));  // ...but fine for a manual 20 minute run
  EXPECT_TRUE(fitsRemainingTime(0, 1));                               // unknown size: let it try
}

TEST(LibrarySyncCore, ErrorLogCountsRotatesAndSummarises) {
  std::string log;
  EXPECT_EQ(unreadCount(log, 0), 0u);
  log +=
      formatErrorLine("2026-11-09 08:14", FailReason::NOT_EPUB, "Twenty Years Later by Charlie Donlea.epub", 1, 784329);
  log += formatErrorLine("2026-11-09 20:30", FailReason::NO_START, "Hidden\tPictures\n.epub", 3, 118501344);
  EXPECT_EQ(countLines(log), 2u);
  EXPECT_EQ(unreadCount(log, 0), 2u);
  EXPECT_EQ(unreadCount(log, 1), 1u);
  EXPECT_EQ(unreadCount(log, 5), 0u);  // a seen-marker ahead of the log never goes negative
  // A tab or newline inside a book name cannot break the one-line format.
  EXPECT_NE(log.find("Hidden Pictures .epub"), std::string::npos);
  EXPECT_EQ(lastErrorSummary(log), "Hidden Pictures .epub - no data within 25 s");
  // Rotation drops the oldest lines and keeps the marker pointing at the same lines.
  std::string big;
  for (int i = 0; i < 10; ++i) big += formatErrorLine("t", FailReason::HTTP_ERROR, "book" + std::to_string(i), 1, 1);
  size_t seen = 4;
  rotateLog(big, seen, 6);
  EXPECT_EQ(countLines(big), 6u);
  EXPECT_EQ(seen, 0u);  // all four read lines were the ones dropped
  EXPECT_EQ(unreadCount(big, seen), 6u);
  EXPECT_NE(big.find("book9"), std::string::npos);
  EXPECT_EQ(big.find("book3\t"), std::string::npos);
  // Summary is cut on a UTF-8 boundary.
  const std::string utf = formatErrorLine("t", FailReason::STALLED, std::string(55, 'a') + "\xC3\xA9\xC3\xA9", 1, 1);
  const std::string cut = lastErrorSummary(utf, 60);
  EXPECT_LE(cut.size(), 60u);
  EXPECT_EQ(static_cast<unsigned char>(cut.back()) & 0xC0, static_cast<unsigned char>(cut.back()) & 0x80 ? 0x80u : 0u);
}
