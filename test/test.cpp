//g++ test.cpp -o leveldb_test ../build/libleveldb.a -lpthread -I../include/ -std=c++11

#include <iostream>
#include <cassert>
#include <cstdlib>
#include <unistd.h>
#include <string>
#include <leveldb/db.h>

using namespace std;

int main(void)
{
    leveldb::DB *db = nullptr;
    leveldb::Options options;
    options.create_if_missing = true;

    // 打开数据库
    leveldb::Status status = leveldb::DB::Open(options, "mydb", &db);
    assert(status.ok());

    // 插入数据
    for (unsigned long long i = 0; i < 1000 * 300; i++) {
        std::string key = "key" + to_string(i);
        std::string value = "value_" + to_string(i);
        std::string get_value;
        
        // 写入 key1 -> value1
        leveldb::Status s = db->Put(leveldb::WriteOptions(), key, value);

        // 写入成功，就读取 key:people 对应的 value
        if (s.ok())
            s = db->Get(leveldb::ReadOptions(), key, &get_value);
    }

    sleep(1);

    // 遍历数据库
    leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      cout << it->key().ToString() << ": "  << it->value().ToString() << endl;
    }
    assert(it->status().ok());  // Check for any errors found during the scan
    delete it;    

    delete db;

    return 0;
}