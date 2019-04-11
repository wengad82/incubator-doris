// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <string>
#include <sstream>
#include <fstream>

#include "gtest/gtest.h"
#include "gmock/gmock.h"
#include "olap/data_dir.h"
#include "olap/tablet_meta_manager.h"
#include "olap/olap_define.h"
#include "boost/filesystem.hpp"
#include "json2pb/json_to_pb.h"

#ifndef BE_TEST
#define BE_TEST
#endif

using ::testing::_;
using ::testing::Return;
using ::testing::SetArgPointee;
using std::string;

namespace doris {

const std::string header_path = "./be/test/olap/test_data/header.txt";

class TabletMetaManagerTest : public testing::Test {
public:
    virtual void SetUp() {
        std::string root_path = "./store";
        ASSERT_TRUE(boost::filesystem::create_directory(root_path));
        _data_dir = new(std::nothrow) DataDir(root_path);
        ASSERT_NE(nullptr, _data_dir);
        Status st = _data_dir->init();
        ASSERT_TRUE(st.ok());
        ASSERT_TRUE(boost::filesystem::exists("./store/meta"));

        std::ifstream infile(header_path);
        char buffer[1024];
        while (!infile.eof()) {
            infile.getline(buffer, 1024);
            _json_header = _json_header + buffer + "\n";
        }
        _json_header = _json_header.substr(0, _json_header.size() - 1);
        _json_header = _json_header.substr(0, _json_header.size() - 1);
    }

    virtual void TearDown() {
        delete _data_dir;
        ASSERT_TRUE(boost::filesystem::remove_all("./store"));
    }

private:
    DataDir* _data_dir;
    std::string _json_header;
};

TEST_F(TabletMetaManagerTest, TestSaveAndGetAndRemove) {
    const TTabletId tablet_id = 15672;
    const TSchemaHash schema_hash = 567997577;
    TabletMetaPB tablet_meta_pb;
    bool ret = json2pb::JsonToProtoMessage(_json_header, &tablet_meta_pb);
    ASSERT_TRUE(ret);
    TabletMetaSharedPtr tablet_meta(new TabletMeta());
    OLAPStatus s = tablet_meta->init_from_pb(tablet_meta_pb);
    ASSERT_EQ(OLAP_SUCCESS, s);
    s = TabletMetaManager::save(_data_dir, tablet_id, schema_hash, tablet_meta);
    ASSERT_EQ(OLAP_SUCCESS, s);
    std::string json_header_read;
    s = TabletMetaManager::get_json_header(_data_dir, tablet_id, schema_hash, &json_header_read);
    ASSERT_EQ(OLAP_SUCCESS, s);
    ASSERT_EQ(_json_header, json_header_read);
    s = TabletMetaManager::remove(_data_dir, tablet_id, schema_hash);
    ASSERT_EQ(OLAP_SUCCESS, s);
    TabletMetaSharedPtr header_read(new TabletMeta());
    s = TabletMetaManager::get_header(_data_dir, tablet_id, schema_hash, header_read);
    ASSERT_EQ(OLAP_ERR_META_KEY_NOT_FOUND, s);
}

TEST_F(TabletMetaManagerTest, TestLoad) {
    const TTabletId tablet_id = 15672;
    const TSchemaHash schema_hash = 567997577;
    OLAPStatus s = TabletMetaManager::load_json_header(_data_dir, header_path);
    ASSERT_EQ(OLAP_SUCCESS, s);
    std::string json_header_read;
    s = TabletMetaManager::get_json_header(_data_dir, tablet_id, schema_hash, &json_header_read);
    ASSERT_EQ(OLAP_SUCCESS, s);
    ASSERT_EQ(_json_header, json_header_read);
}

}  // namespace doris

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
