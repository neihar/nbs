#include "persistent_dynamic_table.h"

#include <library/cpp/testing/unittest/registar.h>

#include <util/folder/tempdir.h>
#include <util/generic/map.h>
#include <util/generic/string.h>
#include <util/generic/vector.h>
#include <util/random/random.h>
#include <util/string/builder.h>

#include <cstring>
#include <memory>

namespace NCloud {

namespace {

////////////////////////////////////////////////////////////////////////////////

struct TTestHeader
{
    ui64 Val = 0;
};

struct TTestData
{
    ui64 Id;
    TString Name;
    TVector<ui32> Values;

    TString Serialize() const
    {
        TString result;
        result.reserve(
            sizeof(Id) + sizeof(ui32) + Name.size() + sizeof(ui32) +
            Values.size() * sizeof(ui32));

        result.append(reinterpret_cast<const char*>(&Id), sizeof(Id));

        ui32 nameLen = Name.size();
        result.append(reinterpret_cast<const char*>(&nameLen), sizeof(nameLen));
        result.append(Name.data(), Name.size());

        ui32 valuesCount = Values.size();
        result.append(
            reinterpret_cast<const char*>(&valuesCount),
            sizeof(valuesCount));
        result.append(
            reinterpret_cast<const char*>(Values.data()),
            Values.size() * sizeof(ui32));

        return result;
    }

    static TTestData Deserialize(const char* data, size_t size)
    {
        TTestData result;
        const char* ptr = data;

        if (size < sizeof(ui64)) {
            return result;
        }
        result.Id = *reinterpret_cast<const ui64*>(ptr);
        ptr += sizeof(ui64);
        size -= sizeof(ui64);

        if (size < sizeof(ui32)) {
            return result;
        }
        ui32 nameLen = *reinterpret_cast<const ui32*>(ptr);
        ptr += sizeof(ui32);
        size -= sizeof(ui32);

        if (size < nameLen) {
            return result;
        }
        result.Name = TString(ptr, nameLen);
        ptr += nameLen;
        size -= nameLen;

        if (size < sizeof(ui32)) {
            return result;
        }
        ui32 valuesCount = *reinterpret_cast<const ui32*>(ptr);
        ptr += sizeof(ui32);
        size -= sizeof(ui32);

        if (size < valuesCount * sizeof(ui32)) {
            return result;
        }
        result.Values.resize(valuesCount);
        if (valuesCount > 0) {
            std::memcpy(result.Values.data(), ptr, valuesCount * sizeof(ui32));
        }

        return result;
    }

    bool operator==(const TTestData& other) const
    {
        return Id == other.Id && Name == other.Name && Values == other.Values;
    }
};

struct TDynamicReferenceImplementation
{
    ui32 MaxTableSize;
    ui32 NextFreeRecord;
    TDeque<ui64> FreeRecords;
    TVector<TString> Records;

    TDynamicReferenceImplementation(ui32 tableSize)
        : MaxTableSize(tableSize)
        , NextFreeRecord(0)
        , Records(tableSize)
    {}

    size_t CountRecords()
    {
        return NextFreeRecord - FreeRecords.size();
    }

    ui64 AllocRecord(ui64 dataSize)
    {
        if (dataSize == 0) {
            return TPersistentDynamicTable<TTestHeader>::InvalidIndex;
        }

        if (!FreeRecords.empty()) {
            ui64 index = FreeRecords.front();
            FreeRecords.pop_front();
            return index;
        }

        if (NextFreeRecord < MaxTableSize) {
            return NextFreeRecord++;
        }

        return TPersistentDynamicTable<TTestHeader>::InvalidIndex;
    }

    void CommitRecord(ui64 index, const TString& data)
    {
        Records[index] = data;
    }

    void DeleteRecord(ui64 index)
    {
        UNIT_ASSERT(!Records[index].empty());
        Records[index].clear();

        if (index + 1 == NextFreeRecord) {
            NextFreeRecord--;
        } else {
            FreeRecords.push_back(index);
        }
    }

    TString GetRecord(ui64 index)
    {
        if (index < Records.size()) {
            return Records[index];
        }
        return TString();
    }

    ui64 SomeRecord()
    {
        TVector<ui64> validIndices;
        for (ui32 i = 0; i < NextFreeRecord; ++i) {
            if (!Records[i].empty()) {
                validIndices.push_back(i);
            }
        }

        if (validIndices.empty()) {
            return TPersistentDynamicTable<TTestHeader>::InvalidIndex;
        }

        return validIndices[RandomNumber(validIndices.size())];
    }

    void Compact()
    {
        int writeIndex = 0;
        for (ui32 i = 0; i < NextFreeRecord; ++i) {
            if (!Records[i].empty()) {
                Records[writeIndex++] = Records[i];
            }
        }

        NextFreeRecord = writeIndex;
        FreeRecords.clear();
    }
};

////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TPersistentDynamicTableTest)
{
    Y_UNIT_TEST(ShouldCreateEmptyTable)
    {
        TTempDir tempDir;
        TString tablePath = tempDir.Path() / "test.table";

        {
            TPersistentDynamicTable<TTestHeader> table(tablePath, 32);
            UNIT_ASSERT_VALUES_EQUAL(0, table.CountRecords());

            auto* header = table.HeaderData();
            UNIT_ASSERT(header != nullptr);
            header->Val = 42;
        }

        {
            TPersistentDynamicTable<TTestHeader> table(tablePath, 32);
            UNIT_ASSERT_VALUES_EQUAL(0, table.CountRecords());
            UNIT_ASSERT_VALUES_EQUAL(42, table.HeaderData()->Val);
        }
    }

    Y_UNIT_TEST(ShouldAllocAndStoreVariableSizeRecords)
    {
        TTempDir tempDir;
        TString tablePath = tempDir.Path() / "test.table";

        TPersistentDynamicTable<TTestHeader> table(tablePath, 32);

        TTestData smallData{1, "small", {1, 2, 3}};
        TTestData largeData{
            2,
            "large_name_with_more_characters",
            {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}};

        TString smallSerialized = smallData.Serialize();
        TString largeSerialized = largeData.Serialize();

        ui64 smallIndex = table.AllocRecord(smallSerialized.size());
        UNIT_ASSERT(
            smallIndex != TPersistentDynamicTable<TTestHeader>::InvalidIndex);
        UNIT_ASSERT(table.WriteRecordData(
            smallIndex,
            smallSerialized.data(),
            smallSerialized.size()));
        table.CommitRecord(smallIndex);

        ui64 largeIndex = table.AllocRecord(largeSerialized.size());
        UNIT_ASSERT(
            largeIndex != TPersistentDynamicTable<TTestHeader>::InvalidIndex);
        UNIT_ASSERT(table.WriteRecordData(
            largeIndex,
            largeSerialized.data(),
            largeSerialized.size()));
        table.CommitRecord(largeIndex);

        UNIT_ASSERT_VALUES_EQUAL(2, table.CountRecords());

        TStringBuf smallRecord = table.GetRecordWithValidation(smallIndex);
        UNIT_ASSERT(!smallRecord.empty());
        TTestData recoveredSmall =
            TTestData::Deserialize(smallRecord.data(), smallRecord.size());
        UNIT_ASSERT(recoveredSmall == smallData);

        TStringBuf largeRecord = table.GetRecordWithValidation(largeIndex);
        UNIT_ASSERT(!largeRecord.empty());
        TTestData recoveredLarge =
            TTestData::Deserialize(largeRecord.data(), largeRecord.size());
        UNIT_ASSERT(recoveredLarge == largeData);
    }

    Y_UNIT_TEST(ShouldIterateOverStoredRecords)
    {
        TTempDir tempDir;
        TString tablePath = tempDir.Path() / "test.table";

        TPersistentDynamicTable<TTestHeader> table(tablePath, 32);

        TVector<TTestData> testRecords = {
            {1, "first", {1, 2}},
            {2, "second", {3, 4, 5}},
            {3, "third", {6, 7, 8, 9}}};

        TVector<ui64> indices;
        for (const auto& testData: testRecords) {
            TString serialized = testData.Serialize();
            ui64 index = table.AllocRecord(serialized.size());
            UNIT_ASSERT(
                index != TPersistentDynamicTable<TTestHeader>::InvalidIndex);
            UNIT_ASSERT(table.WriteRecordData(
                index,
                serialized.data(),
                serialized.size()));
            table.CommitRecord(index);
            indices.push_back(index);
        }

        size_t count = 0;
        for (auto it = table.begin(); it != table.end(); ++it) {
            TStringBuf record = *it;

            UNIT_ASSERT(!record.empty());

            TTestData recovered =
                TTestData::Deserialize(record.data(), record.size());

            UNIT_ASSERT(recovered == testRecords[count]);

            ++count;
        }

        UNIT_ASSERT_VALUES_EQUAL(testRecords.size(), count);
    }

    Y_UNIT_TEST(ShouldDeleteRecords)
    {
        TTempDir tempDir;
        TString tablePath = tempDir.Path() / "test.table";

        TPersistentDynamicTable<TTestHeader> table(tablePath, 32);

        TTestData data1{1, "first", {1, 2}};
        TTestData data2{2, "second", {3, 4}};

        TString serialized1 = data1.Serialize();
        TString serialized2 = data2.Serialize();

        ui64 index1 = table.AllocRecord(serialized1.size());
        UNIT_ASSERT(table.WriteRecordData(
            index1,
            serialized1.data(),
            serialized1.size()));
        table.CommitRecord(index1);

        ui64 index2 = table.AllocRecord(serialized2.size());
        UNIT_ASSERT(table.WriteRecordData(
            index2,
            serialized2.data(),
            serialized2.size()));
        table.CommitRecord(index2);

        UNIT_ASSERT_VALUES_EQUAL(2, table.CountRecords());

        table.DeleteRecord(index1);
        UNIT_ASSERT_VALUES_EQUAL(1, table.CountRecords());

        UNIT_ASSERT(table.GetRecord(index1).empty());
        UNIT_ASSERT(!table.GetRecord(index2).empty());
    }

    Y_UNIT_TEST(ShouldReuseDeletedRecordSlots)
    {
        TTempDir tempDir;
        TString tablePath = tempDir.Path() / "test.table";

        TPersistentDynamicTable<TTestHeader> table(tablePath, 4);

        TVector<ui64> indices;
        for (int i = 0; i < 4; ++i) {
            TTestData data{
                static_cast<ui64>(i),
                TStringBuilder() << "item" << i,
                {static_cast<ui32>(i)}};
            TString serialized = data.Serialize();
            ui64 index = table.AllocRecord(serialized.size());
            UNIT_ASSERT(
                index != TPersistentDynamicTable<TTestHeader>::InvalidIndex);
            UNIT_ASSERT(table.WriteRecordData(
                index,
                serialized.data(),
                serialized.size()));
            table.CommitRecord(index);
            indices.push_back(index);
        }

        UNIT_ASSERT_VALUES_EQUAL(4, table.CountRecords());

        TTestData extraData{999, "extra", {999}};
        TString extraSerialized = extraData.Serialize();
        ui64 extraIndex = table.AllocRecord(extraSerialized.size());
        UNIT_ASSERT_VALUES_EQUAL(
            TPersistentDynamicTable<TTestHeader>::InvalidIndex,
            extraIndex);

        table.DeleteRecord(indices[1]);
        UNIT_ASSERT_VALUES_EQUAL(3, table.CountRecords());

        ui64 newIndex = table.AllocRecord(extraSerialized.size());
        UNIT_ASSERT(
            newIndex != TPersistentDynamicTable<TTestHeader>::InvalidIndex);
        UNIT_ASSERT_VALUES_EQUAL(indices[1], newIndex);
    }

    Y_UNIT_TEST(ShouldPersistAcrossRestarts)
    {
        TTempDir tempDir;
        TString tablePath = tempDir.Path() / "test.table";

        TVector<TTestData> testData = {
            {100, "persistent_first", {10, 20, 30}},
            {200, "persistent_second", {40, 50, 60, 70}}};

        {
            TPersistentDynamicTable<TTestHeader> table(tablePath, 32);
            table.HeaderData()->Val = 123;

            for (const auto& data: testData) {
                TString serialized = data.Serialize();
                ui64 index = table.AllocRecord(serialized.size());
                UNIT_ASSERT(table.WriteRecordData(
                    index,
                    serialized.data(),
                    serialized.size()));
                table.CommitRecord(index);
            }

            UNIT_ASSERT_VALUES_EQUAL(testData.size(), table.CountRecords());
        }

        {
            TPersistentDynamicTable<TTestHeader> table(tablePath, 32);
            UNIT_ASSERT_VALUES_EQUAL(123, table.HeaderData()->Val);
            UNIT_ASSERT_VALUES_EQUAL(testData.size(), table.CountRecords());

            THashSet<ui64> foundIds;
            for (auto it = table.begin(); it != table.end(); ++it) {
                TStringBuf record = *it;

                TTestData recovered =
                    TTestData::Deserialize(record.data(), record.size());
                foundIds.insert(recovered.Id);

                bool found = false;
                for (const auto& original: testData) {
                    if (recovered == original) {
                        found = true;
                        break;
                    }
                }
                UNIT_ASSERT(found);
            }

            UNIT_ASSERT_VALUES_EQUAL(testData.size(), foundIds.size());
        }
    }

    Y_UNIT_TEST(ShouldMakeCorrectCompaction)
    {
        TTempDir tempDir;
        TString tablePath = tempDir.Path() / "test.table";

        TVector<TTestData> testData = {
            {100, "persistent_first", {10, 20, 30}},
            {200, "persistent_second", {40, 50, 60, 70}},
            {300, "persistent_third", {80, 90, 100, 110, 120}}};

        ui64 index1 = TPersistentDynamicTable<TTestHeader>::InvalidIndex;
        ui64 index2 = TPersistentDynamicTable<TTestHeader>::InvalidIndex;

        {
            TPersistentDynamicTable<TTestHeader> table(tablePath, 32);
            table.HeaderData()->Val = 123;

            TString serialized = testData[0].Serialize();
            index1 = table.AllocRecord(serialized.size());
            UNIT_ASSERT(table.WriteRecordData(
                index1,
                serialized.data(),
                serialized.size()));
            table.CommitRecord(index1);

            serialized = testData[1].Serialize();
            index2 = table.AllocRecord(serialized.size());
            UNIT_ASSERT(table.WriteRecordData(
                index2,
                serialized.data(),
                serialized.size()));
            table.CommitRecord(index2);

            table.DeleteRecord(index1);

            serialized = testData[2].Serialize();
            ui64 index3 = table.AllocRecord(serialized.size());
            UNIT_ASSERT(table.WriteRecordData(
                index3,
                serialized.data(),
                serialized.size()));
            table.CommitRecord(index3);

            UNIT_ASSERT_VALUES_EQUAL(index3, index1);

            UNIT_ASSERT_VALUES_EQUAL(2, table.CountRecords());
        }

        {
            TPersistentDynamicTable<TTestHeader> table(tablePath, 32);
            UNIT_ASSERT_VALUES_EQUAL(123, table.HeaderData()->Val);
            UNIT_ASSERT_VALUES_EQUAL(2, table.CountRecords());

            TStringBuf record = table.GetRecord(index1);

            TTestData recovered =
                TTestData::Deserialize(record.data(), record.size());

            UNIT_ASSERT(recovered == testData[2]);

            record = table.GetRecord(index2);
            recovered = TTestData::Deserialize(record.data(), record.size());
            UNIT_ASSERT(recovered == testData[1]);
        }
    }

    Y_UNIT_TEST(ShouldHandleDataAreaExpansion)
    {
        TTempDir tempDir;
        TString tablePath = tempDir.Path() / "test.table";

        TPersistentDynamicTable<TTestHeader> table(tablePath, 100, 1024);

        TVector<ui64> indices;
        for (int i = 0; i < 50; ++i) {
            TTestData data{
                static_cast<ui64>(i),
                TStringBuilder()
                    << "large_record_name_" << i << "_with_long_suffix",
                TVector<ui32>(100, i)};

            TString serialized = data.Serialize();
            ui64 index = table.AllocRecord(serialized.size());
            UNIT_ASSERT(
                index != TPersistentDynamicTable<TTestHeader>::InvalidIndex);
            UNIT_ASSERT(table.WriteRecordData(
                index,
                serialized.data(),
                serialized.size()));
            table.CommitRecord(index);
            indices.push_back(index);
        }

        UNIT_ASSERT_VALUES_EQUAL(50, table.CountRecords());

        for (size_t i = 0; i < indices.size(); ++i) {
            TStringBuf record = table.GetRecord(indices[i]);
            UNIT_ASSERT(!record.empty());

            TTestData recovered =
                TTestData::Deserialize(record.data(), record.size());
            UNIT_ASSERT_VALUES_EQUAL(i, recovered.Id);
        }
    }

    Y_UNIT_TEST(ShouldTriggerFileMapExpansionWhenDataAreaFull)
    {
        TTempDir tempDir;
        TString tablePath = tempDir.Path() / "test.table";

        TPersistentDynamicTable<TTestHeader> table(tablePath, 10, 256);

        TVector<ui64> indices;
        size_t totalUsed = 0;

        for (int i = 0; i < 20; ++i) {
            TTestData data{
                static_cast<ui64>(i),
                TStringBuilder() << "record" << i,
                {static_cast<ui32>(i)}};
            TString serialized = data.Serialize();

            ui64 index = table.AllocRecord(serialized.size());
            if (index == TPersistentDynamicTable<TTestHeader>::InvalidIndex) {
                break;
            }

            UNIT_ASSERT(table.WriteRecordData(
                index,
                serialized.data(),
                serialized.size()));
            table.CommitRecord(index);
            indices.push_back(index);
            totalUsed += serialized.size();
        }

        // Test verifies automatic file expansion beyond initial 256 bytes
        UNIT_ASSERT(totalUsed > 256);
        UNIT_ASSERT(indices.size() > 5);

        for (size_t i = 0; i < indices.size(); ++i) {
            TStringBuf record = table.GetRecord(indices[i]);
            UNIT_ASSERT(!record.empty());

            TTestData recovered =
                TTestData::Deserialize(record.data(), record.size());
            UNIT_ASSERT_VALUES_EQUAL(i, recovered.Id);
        }
    }

    Y_UNIT_TEST(ShouldUpdateRecordWithDecreasedSize)
    {
        TTempDir tempDir;
        TString tablePath = tempDir.Path() / "test.table";

        TPersistentDynamicTable<TTestHeader> table(tablePath, 32);

        TTestData originalData{
            1,
            "original_large_name_with_extra_content",
            {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}};
        TString originalSerialized = originalData.Serialize();

        ui64 index = table.AllocRecord(originalSerialized.size());
        UNIT_ASSERT(
            index != TPersistentDynamicTable<TTestHeader>::InvalidIndex);
        UNIT_ASSERT(table.WriteRecordData(
            index,
            originalSerialized.data(),
            originalSerialized.size()));
        table.CommitRecord(index);

        TStringBuf originalRecord = table.GetRecord(index);
        UNIT_ASSERT_VALUES_EQUAL(
            originalSerialized.size(),
            originalRecord.size());

        TTestData updatedData{1, "small", {1, 2}};
        TString updatedSerialized = updatedData.Serialize();
        UNIT_ASSERT(updatedSerialized.size() < originalSerialized.size());

        UNIT_ASSERT(table.WriteRecordData(
            index,
            updatedSerialized.data(),
            updatedSerialized.size()));

        TStringBuf updatedRecord = table.GetRecord(index);
        UNIT_ASSERT_VALUES_EQUAL(
            updatedSerialized.size(),
            updatedRecord.size());

        TTestData recoveredData =
            TTestData::Deserialize(updatedRecord.data(), updatedRecord.size());
        UNIT_ASSERT(recoveredData == updatedData);
    }

    Y_UNIT_TEST(ShouldCompactDataAreaAndReuseSpace)
    {
        TTempDir tempDir;
        TString tablePath = tempDir.Path() / "test.table";

        TPersistentDynamicTable<TTestHeader> table(tablePath, 20, 512);

        TVector<ui64> indices;
        for (int i = 0; i < 10; ++i) {
            TTestData data{
                static_cast<ui64>(i),
                TStringBuilder() << "record_" << i << "_with_content",
                {static_cast<ui32>(i), static_cast<ui32>(i + 1)}};
            TString serialized = data.Serialize();

            ui64 index = table.AllocRecord(serialized.size());
            if (index == TPersistentDynamicTable<TTestHeader>::InvalidIndex) {
                break;
            }

            UNIT_ASSERT(table.WriteRecordData(
                index,
                serialized.data(),
                serialized.size()));
            table.CommitRecord(index);
            indices.push_back(index);
        }

        size_t recordsBeforeDelete = table.CountRecords();
        UNIT_ASSERT(recordsBeforeDelete > 3);

        // Create fragmentation by deleting every other record
        for (size_t i = 1; i < indices.size(); i += 2) {
            table.DeleteRecord(indices[i]);
        }

        size_t recordsAfterDelete = table.CountRecords();
        UNIT_ASSERT(recordsAfterDelete < recordsBeforeDelete);

        // Test that large record fits after compaction despite fragmentation
        TTestData largeData{
            999,
            "large_record_that_needs_compaction_to_fit_in_available_space",
            TVector<ui32>(20, 999)};
        TString largeSerialized = largeData.Serialize();

        ui64 newIndex = table.AllocRecord(largeSerialized.size());
        UNIT_ASSERT(
            newIndex != TPersistentDynamicTable<TTestHeader>::InvalidIndex);
        UNIT_ASSERT(table.WriteRecordData(
            newIndex,
            largeSerialized.data(),
            largeSerialized.size()));
        table.CommitRecord(newIndex);

        TStringBuf largeRecord = table.GetRecord(newIndex);
        UNIT_ASSERT(!largeRecord.empty());

        TTestData recovered =
            TTestData::Deserialize(largeRecord.data(), largeRecord.size());
        UNIT_ASSERT(recovered == largeData);

        for (size_t i = 0; i < indices.size(); i += 2) {
            TStringBuf remainingRecord = table.GetRecord(indices[i]);
            UNIT_ASSERT(!remainingRecord.empty());
        }
    }

    Y_UNIT_TEST(ShouldFailWhenMaxRecordsLimitReached)
    {
        TTempDir tempDir;
        TString tablePath = tempDir.Path() / "test.table";

        TPersistentDynamicTable<TTestHeader> table(tablePath, 3, 1024);

        TVector<ui64> indices;

        for (int i = 0; i < 3; ++i) {
            TTestData data{
                static_cast<ui64>(i),
                TStringBuilder() << "record" << i,
                {static_cast<ui32>(i)}};
            TString serialized = data.Serialize();

            ui64 index = table.AllocRecord(serialized.size());
            UNIT_ASSERT(
                index != TPersistentDynamicTable<TTestHeader>::InvalidIndex);
            UNIT_ASSERT(table.WriteRecordData(
                index,
                serialized.data(),
                serialized.size()));
            table.CommitRecord(index);
            indices.push_back(index);
        }

        UNIT_ASSERT_VALUES_EQUAL(3, table.CountRecords());

        TTestData extraData{999, "extra", {999}};
        TString extraSerialized = extraData.Serialize();
        ui64 extraIndex = table.AllocRecord(extraSerialized.size());
        UNIT_ASSERT_VALUES_EQUAL(
            TPersistentDynamicTable<TTestHeader>::InvalidIndex,
            extraIndex);

        UNIT_ASSERT_VALUES_EQUAL(3, table.CountRecords());
        for (size_t i = 0; i < indices.size(); ++i) {
            TStringBuf record = table.GetRecord(indices[i]);
            UNIT_ASSERT(!record.empty());
        }

        // Test slot reuse after deletion
        table.DeleteRecord(indices[1]);
        UNIT_ASSERT_VALUES_EQUAL(2, table.CountRecords());

        ui64 newIndex = table.AllocRecord(extraSerialized.size());
        UNIT_ASSERT(
            newIndex != TPersistentDynamicTable<TTestHeader>::InvalidIndex);
        UNIT_ASSERT_VALUES_EQUAL(indices[1], newIndex);
        UNIT_ASSERT(table.WriteRecordData(
            newIndex,
            extraSerialized.data(),
            extraSerialized.size()));
        table.CommitRecord(newIndex);

        UNIT_ASSERT_VALUES_EQUAL(3, table.CountRecords());
    }

    Y_UNIT_TEST(ShouldHandleZeroSizeData)
    {
        TTempDir tempDir;
        TString tablePath = tempDir.Path() / "test.table";

        TPersistentDynamicTable<TTestHeader> table(tablePath, 32);

        ui64 index = table.AllocRecord(0);
        UNIT_ASSERT(
            index == TPersistentDynamicTable<TTestHeader>::InvalidIndex);

        TTestData emptyData{42, "", {}};
        TString emptySerialized = emptyData.Serialize();

        ui64 emptyIndex = table.AllocRecord(emptySerialized.size());
        UNIT_ASSERT(
            emptyIndex != TPersistentDynamicTable<TTestHeader>::InvalidIndex);
        UNIT_ASSERT(table.WriteRecordData(
            emptyIndex,
            emptySerialized.data(),
            emptySerialized.size()));
        table.CommitRecord(emptyIndex);

        TStringBuf emptyRecord = table.GetRecord(emptyIndex);
        UNIT_ASSERT(!emptyRecord.empty());
        TTestData recoveredEmpty =
            TTestData::Deserialize(emptyRecord.data(), emptyRecord.size());
        UNIT_ASSERT(recoveredEmpty == emptyData);

        UNIT_ASSERT_VALUES_EQUAL(1, table.CountRecords());

        TTestData normalData{99, "normal", {1, 2, 3}};
        TString normalSerialized = normalData.Serialize();

        ui64 normalIndex = table.AllocRecord(normalSerialized.size());
        UNIT_ASSERT(
            !table.WriteRecordData(normalIndex, normalSerialized.data(), 0));
        UNIT_ASSERT(!table.WriteRecordData(
            normalIndex,
            nullptr,
            normalSerialized.size()));
        UNIT_ASSERT(table.WriteRecordData(
            normalIndex,
            normalSerialized.data(),
            normalSerialized.size()));
        table.CommitRecord(normalIndex);

        TStringBuf normalRecord = table.GetRecord(normalIndex);
        UNIT_ASSERT_VALUES_EQUAL(normalSerialized.size(), normalRecord.size());

        UNIT_ASSERT_VALUES_EQUAL(2, table.CountRecords());
    }

    Y_UNIT_TEST(RandomizedAllocDeleteRestore)
    {
        TTempDir tempDir;
        TString tablePath = tempDir.Path() / "test.table";

        const ui32 tableSize = 50;
        const ui32 testRecords = 2000;
        const double restoreProbability = 0.05;

        std::unique_ptr<TPersistentDynamicTable<TTestHeader>> table;
        TDynamicReferenceImplementation ri(tableSize);

        auto restore = [&]()
        {
            table = std::make_unique<TPersistentDynamicTable<TTestHeader>>(
                tablePath,
                tableSize,
                1024);
        };

        restore();

        ui32 remainingRecords = testRecords;
        while (remainingRecords || ri.CountRecords()) {
            const bool shouldAlloc = remainingRecords && RandomNumber<bool>();

            if (shouldAlloc) {
                TTestData testData{
                    RandomNumber<ui64>(),
                    TStringBuilder() << "record_" << RandomNumber<ui32>(1000),
                    TVector<ui32>()};

                ui32 valueCount = RandomNumber<ui32>(15);
                for (ui32 i = 0; i < valueCount; ++i) {
                    testData.Values.push_back(RandomNumber<ui32>());
                }

                TString serialized = testData.Serialize();
                ui64 tableIndex = table->AllocRecord(serialized.size());
                ui64 riIndex = ri.AllocRecord(serialized.size());

                UNIT_ASSERT_VALUES_EQUAL(tableIndex, riIndex);

                if (tableIndex !=
                    TPersistentDynamicTable<TTestHeader>::InvalidIndex)
                {
                    remainingRecords--;

                    UNIT_ASSERT(table->WriteRecordData(
                        tableIndex,
                        serialized.data(),
                        serialized.size()));
                    table->CommitRecord(tableIndex);
                    ri.CommitRecord(riIndex, serialized);
                }
            } else {
                if (ri.CountRecords()) {
                    ui64 index = ri.SomeRecord();
                    if (index !=
                        TPersistentDynamicTable<TTestHeader>::InvalidIndex)
                    {
                        TStringBuf tableRecord = table->GetRecord(index);
                        TString riRecord = ri.GetRecord(index);

                        if (!tableRecord.empty() && !riRecord.empty()) {
                            UNIT_ASSERT_VALUES_EQUAL(
                                tableRecord.size(),
                                riRecord.size());
                            UNIT_ASSERT_VALUES_EQUAL(
                                TString(tableRecord),
                                riRecord);
                        }

                        ri.DeleteRecord(index);
                        table->DeleteRecord(index);
                    }
                }
            }

            if (RandomNumber<double>() < restoreProbability) {
                restore();
                ri.Compact();
            }

            UNIT_ASSERT_VALUES_EQUAL(ri.CountRecords(), table->CountRecords());
        }
    }
}

}   // namespace

}   // namespace NCloud
