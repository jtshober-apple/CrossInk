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
  EXPECT_EQ(decide(line, &same, false, true), Decision::UP_TO_DATE);
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
