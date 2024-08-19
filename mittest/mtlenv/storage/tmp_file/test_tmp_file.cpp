/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#include "mittest/mtlenv/storage/tmp_file/ob_tmp_file_test_helper.h"
#define USING_LOG_PREFIX STORAGE
#define protected public
#define private public
#include "mittest/mtlenv/mock_tenant_module_env.h"
#include "share/ob_simple_mem_limit_getter.h"
#include "lib/alloc/ob_malloc_allocator.h"
#include "storage/tmp_file/ob_tmp_file_global.h"
#include "storage/tmp_file/ob_tmp_file_meta_tree.h"

namespace oceanbase
{
using namespace common;
using namespace blocksstable;
using namespace tmp_file;
using namespace storage;
using namespace share::schema;
/* ------------------------------ Mock Parameter ---------------------------- */
static const int64_t TENANT_MEMORY = 8L * 1024L * 1024L * 1024L /* 8 GB */;
/********************************* Mock WBP *************************** */
static const int64_t WBP_BLOCK_SIZE = ObTmpWriteBufferPool::WBP_BLOCK_SIZE; // each wbp block has 253 pages (253 * 8KB == 2024KB)
static const int64_t SMALL_WBP_BLOCK_COUNT = 3;
static const int64_t SMALL_WBP_MEM_LIMIT = SMALL_WBP_BLOCK_COUNT * WBP_BLOCK_SIZE; // the wbp mem size is 5.93MB
static const int64_t BIG_WBP_BLOCK_COUNT = 40;
static const int64_t BIG_WBP_MEM_LIMIT = BIG_WBP_BLOCK_COUNT * WBP_BLOCK_SIZE; // the wbp mem size is 79.06MB
/********************************* Mock WBP Index Cache*************************** */
// each bucket could indicate a 256KB data in wbp.
// SMALL_WBP_IDX_CACHE_MAX_CAPACITY will indicate 4MB data in wbp
static const int64_t SMALL_WBP_IDX_CACHE_MAX_CAPACITY = ObTmpFileWBPIndexCache::INIT_BUCKET_ARRAY_CAPACITY * 2;
/********************************* Mock Meta Tree *************************** */
static const int64_t MAX_DATA_ITEM_ARRAY_COUNT = 2;
static const int64_t MAX_PAGE_ITEM_COUNT = 4;   // MAX_PAGE_ITEM_COUNT * ObTmpFileGlobal::PAGE_SIZE means
                                                // the max representation range of a meta page (4 * 2MB == 8MB).
                                                // according to the formula of summation for geometric sequence
                                                // (S_n = a_1 * (1-q^n)/(1-q), where a_1 = 8MB, q = 4),
                                                // a two-level meta tree could represent at most 40MB disk data of tmp file
                                                // a three-level meta tree could represent at most 168MB disk data of tmp file
                                                // a four-level meta tree could represent at most 680MB disk data of tmp file

/* ---------------------------- Unittest Class ----------------------------- */

class TestTmpFile : public ::testing::Test
{
public:
  TestTmpFile() = default;
  virtual ~TestTmpFile() = default;
  virtual void SetUp();
  virtual void TearDown();
  static void SetUpTestCase();
  static void TearDownTestCase();
};
static ObSimpleMemLimitGetter getter;
static const int64_t TEST_ROWKEY_COLUMN_CNT = 2;

// ATTENTION!
// currently, we only initialize modules about tmp file at the beginning of unit test and
// never restart them in the end of test case.
// please make sure that all test cases will not affect the others.
void TestTmpFile::SetUpTestCase()
{
  int ret = OB_SUCCESS;
  ASSERT_EQ(OB_SUCCESS, MockTenantModuleEnv::get_instance().init());

  CHUNK_MGR.set_limit(TENANT_MEMORY);
  ObMallocAllocator::get_instance()->set_tenant_limit(MTL_ID(), TENANT_MEMORY);

  MTL(ObTenantTmpFileManager *)->page_cache_controller_.write_buffer_pool_.default_wbp_memory_limit_ = SMALL_WBP_MEM_LIMIT;
  ObSharedNothingTmpFileMetaTree::set_max_array_item_cnt(MAX_DATA_ITEM_ARRAY_COUNT);
  ObSharedNothingTmpFileMetaTree::set_max_page_item_cnt(MAX_PAGE_ITEM_COUNT);
}

void TestTmpFile::SetUp()
{
  int ret = OB_SUCCESS;

  const int64_t bucket_num = 1024L;
  const int64_t max_cache_size = 1024L * 1024L * 512;
  const int64_t block_size = common::OB_MALLOC_BIG_BLOCK_SIZE;

  ASSERT_EQ(true, MockTenantModuleEnv::get_instance().is_inited());
  if (!ObKVGlobalCache::get_instance().inited_) {
    ASSERT_EQ(OB_SUCCESS, ObKVGlobalCache::get_instance().init(&getter,
        bucket_num,
        max_cache_size,
        block_size));
  }
//  if (!MTL(ObTenantTmpFileManager *)->is_inited_) {
//    ret = MTL(ObTenantTmpFileManager *)->init();
//    ASSERT_EQ(OB_SUCCESS, ret);
//    ret = MTL(ObTenantTmpFileManager *)->start();
//    ASSERT_EQ(OB_SUCCESS, ret);
//    MTL(ObTenantTmpFileManager *)->page_cache_controller_.write_buffer_pool_.default_wbp_memory_limit_ = SMALL_WBP_MEM_LIMIT;
//  }
}

void TestTmpFile::TearDownTestCase()
{
  MockTenantModuleEnv::get_instance().destroy();
}

void TestTmpFile::TearDown()
{
  ObKVGlobalCache::get_instance().destroy();
//  if (MTL(ObTenantTmpFileManager *)->is_inited_) {
//    MTL(ObTenantTmpFileManager *)->stop();
//    MTL(ObTenantTmpFileManager *)->wait();
//    MTL(ObTenantTmpFileManager *)->destroy();
//  }
}

// generate 2MB random data (will not trigger flush and evict logic)
// 1. test write pages and append write tail page
// 2. test write after reading
TEST_F(TestTmpFile, test_unaligned_data_read_write)
{
  int ret = OB_SUCCESS;
  const int64_t write_size = 2 * 1024 * 1024;
  const int64_t wbp_mem_limit = MTL(ObTenantTmpFileManager *)->page_cache_controller_.write_buffer_pool_.get_memory_limit();
  ASSERT_LT(write_size, wbp_mem_limit);
  char * write_buffer = new char[write_size];
  for (int64_t i = 0; i < write_size;) {
    int64_t random_length = generate_random_int(1024, 8 * 1024);
    int64_t random_int = generate_random_int(0, 256);
    for (int64_t j = 0; j < random_length && i + j < write_size; ++j) {
      write_buffer[i + j] = random_int;
    }
    i += random_length;
  }
  int64_t dir = -1;
  int64_t fd = -1;
  const int64_t macro_block_size = OB_SERVER_BLOCK_MGR.get_macro_block_size();
  ObTmpFileIOInfo io_info;
  ObTmpFileIOHandle handle;
  ret = MTL(ObTenantTmpFileManager *)->alloc_dir(dir);
  ASSERT_EQ(OB_SUCCESS, ret);
  ret = MTL(ObTenantTmpFileManager *)->open(fd, dir);
  std::cout << "open temporary file: " << fd << std::endl;
  ASSERT_EQ(OB_SUCCESS, ret);
  ObTmpFileHandle file_handle;
  ret = MTL(ObTenantTmpFileManager *)->get_tmp_file(fd, file_handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  file_handle.get()->page_idx_cache_.max_bucket_array_capacity_ = SMALL_WBP_IDX_CACHE_MAX_CAPACITY;
  file_handle.reset();
  // dump random data
  {
    std::string r_file_name = std::to_string(fd) + "_raw_write_data.txt";
    dump_hex_data(write_buffer, write_size, r_file_name);
  }

  // random write, read, and check
  int64_t already_write = 0;
  std::vector<int64_t> turn_write_size = generate_random_sequence(1, write_size / 3, write_size, 3);
  for (int i = 0; i < turn_write_size.size(); ++i) {
    int64_t this_turn_write_size = turn_write_size[i];
    std::cout << "random write and read " << this_turn_write_size << std::endl;
    // write data
    {
      ObTmpFileIOInfo io_info;
      io_info.fd_ = fd;
      io_info.io_desc_.set_wait_event(2);
      io_info.io_timeout_ms_ = DEFAULT_IO_WAIT_TIME_MS;
      io_info.buf_ = write_buffer + already_write;
      if (this_turn_write_size % ObTmpFileGlobal::PAGE_SIZE == 0 && i == 0) {
        io_info.size_ = this_turn_write_size - 2 * 1024;
        ASSERT_EQ(OB_SUCCESS, MTL(ObTenantTmpFileManager *)->write(io_info));

        io_info.size_ = 2 * 1024;
        io_info.buf_ = write_buffer + already_write + this_turn_write_size - 2 * 1024;
        ASSERT_EQ(OB_SUCCESS, MTL(ObTenantTmpFileManager *)->write(io_info));
      } else {
        io_info.size_ = this_turn_write_size;
        ASSERT_EQ(OB_SUCCESS, MTL(ObTenantTmpFileManager *)->write(io_info));
      }
    }
    // read data
    char * read_check_buffer = new char[this_turn_write_size];
    {
      ObTmpFileIOInfo io_info;
      ObTmpFileIOHandle handle;
      io_info.fd_ = fd;
      io_info.size_ = this_turn_write_size;
      io_info.io_desc_.set_wait_event(2);
      io_info.io_timeout_ms_ = DEFAULT_IO_WAIT_TIME_MS;
      io_info.buf_ = read_check_buffer;
      ASSERT_EQ(OB_SUCCESS, MTL(ObTenantTmpFileManager *)->read(io_info, handle));
    }
    // check data
    {
      std::string compare_file_name = std::to_string(fd) + "_compare_result.txt";
      bool is_equal = compare_and_print_hex_data(
          write_buffer + already_write, read_check_buffer,
          this_turn_write_size, 200, compare_file_name);
      if (!is_equal) {
        // dump write data
        std::string w_file_name = std::to_string(fd) + "_write_data.txt";
        dump_hex_data(write_buffer + already_write, this_turn_write_size, w_file_name);
        // dump read check data
        std::string r_file_name = std::to_string(fd) + "_read_data.txt";
        dump_hex_data(read_check_buffer, this_turn_write_size, r_file_name);
        // abort
        std::cout << "not equal in random data test"
                  << "\nwrite dumped file: " << w_file_name
                  << "\nread check dumped file: " << r_file_name
                  << "\ncompare result file: " << compare_file_name << std::endl;
        ob_abort();
      }
    }
    // update already_write
    delete [] read_check_buffer;
    already_write += this_turn_write_size;
  }

  ret = MTL(ObTenantTmpFileManager *)->remove(fd);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(0, MTL(ObTenantTmpFileManager *)->page_cache_controller_.flush_priority_mgr_.get_file_size());
  ASSERT_EQ(0, MTL(ObTenantTmpFileManager *)->page_cache_controller_.evict_mgr_.get_file_size());
  LOG_INFO("test_unaligned_data_read_write");
}

// generate 7MB random data
// this test will trigger flush and evict logic for data pages.
// meta tree will not be evicted in this test.
// 1. test pread
// 1.1 read disk data
// 1.2 read memory data
// 1.3 read both disk and memory data
// 1.4 read OB_ITER_END
// 2. test read
// 2.1 read aligned data
// 2.2 read unaligned data
TEST_F(TestTmpFile, test_read)
{
  int ret = OB_SUCCESS;
  const int64_t write_size = 7 * 1024 * 1024; // 7MB
  const int64_t wbp_mem_limit = MTL(ObTenantTmpFileManager *)->page_cache_controller_.write_buffer_pool_.get_memory_limit();
  ASSERT_GT(write_size, wbp_mem_limit);
  char *write_buf = new char [write_size];
  for (int64_t i = 0; i < write_size;) {
    int64_t random_length = generate_random_int(1024, 8 * 1024);
    int64_t random_int = generate_random_int(0, 256);
    for (int64_t j = 0; j < random_length && i + j < write_size; ++j) {
      write_buf[i + j] = random_int;
    }
    i += random_length;
  }

  int64_t dir = -1;
  int64_t fd = -1;
  ret = MTL(ObTenantTmpFileManager *)->alloc_dir(dir);
  ASSERT_EQ(OB_SUCCESS, ret);
  ret = MTL(ObTenantTmpFileManager *)->open(fd, dir);
  std::cout << "open temporary file: " << fd << std::endl;
  ASSERT_EQ(OB_SUCCESS, ret);
  ObTmpFileHandle file_handle;
  ret = MTL(ObTenantTmpFileManager *)->get_tmp_file(fd, file_handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  file_handle.get()->page_idx_cache_.max_bucket_array_capacity_ = SMALL_WBP_IDX_CACHE_MAX_CAPACITY;
  file_handle.reset();

  ObTmpFileIOInfo io_info;
  io_info.fd_ = fd;
  io_info.io_desc_.set_wait_event(2);
  io_info.buf_ = write_buf;
  io_info.size_ = write_size;
  io_info.io_timeout_ms_ = DEFAULT_IO_WAIT_TIME_MS;
  // Write data
  int64_t write_time = ObTimeUtility::current_time();
  ret = MTL(ObTenantTmpFileManager *)->write(io_info);
  write_time = ObTimeUtility::current_time() - write_time;
  ASSERT_EQ(OB_SUCCESS, ret);

  ret = MTL(ObTenantTmpFileManager *)->get_tmp_file(fd, file_handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  int64_t wbp_begin_offset = file_handle.get()->cal_wbp_begin_offset();
  ASSERT_GT(wbp_begin_offset, 0);
  file_handle.get()->page_idx_cache_.max_bucket_array_capacity_ = SMALL_WBP_IDX_CACHE_MAX_CAPACITY;
  file_handle.reset();

  int64_t read_time = ObTimeUtility::current_time();
  /************** test pread **************/
  // 1. read memory data
  char *read_buf = new char [write_size - wbp_begin_offset];
  ObTmpFileIOHandle handle;
  io_info.buf_ = read_buf;
  io_info.size_ = write_size - wbp_begin_offset;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, wbp_begin_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(io_info.size_, handle.get_done_size());
  int cmp = memcmp(handle.get_buffer(), write_buf + wbp_begin_offset, io_info.size_);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;

  // 2. read disk data
  read_buf = new char [wbp_begin_offset];
  io_info.buf_ = read_buf;
  io_info.size_ = wbp_begin_offset;
  io_info.disable_block_cache_ = true;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, 0, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(io_info.size_, handle.get_done_size());
  cmp = memcmp(handle.get_buffer(), write_buf, io_info.size_);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;

  read_buf = new char [wbp_begin_offset];
  io_info.buf_ = read_buf;
  io_info.size_ = wbp_begin_offset;
  io_info.disable_block_cache_ = false;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, 0, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(io_info.size_, handle.get_done_size());
  cmp = memcmp(handle.get_buffer(), write_buf, io_info.size_);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;

  // 3. read both disk and memory data
  int64_t read_size = wbp_begin_offset / 2 + 9 * 1024;
  int64_t read_offset = wbp_begin_offset / 2 + 1024;
  read_buf = new char [read_size];
  io_info.buf_ = read_buf;
  io_info.size_ = read_size;
  io_info.disable_block_cache_ = true;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(io_info.size_, handle.get_done_size());
  cmp = memcmp(handle.get_buffer(), write_buf + read_offset, io_info.size_);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;

  read_size = wbp_begin_offset / 2 + 9 * 1024;
  read_offset = wbp_begin_offset / 2 + 1024;
  read_buf = new char [read_size];
  io_info.buf_ = read_buf;
  io_info.size_ = read_size;
  io_info.disable_block_cache_ = false;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(io_info.size_, handle.get_done_size());
  cmp = memcmp(handle.get_buffer(), write_buf + read_offset, io_info.size_);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;

  // 4. read OB_ITER_END
  read_buf = new char [200];
  io_info.buf_ = read_buf;
  io_info.size_ = 200;
  io_info.disable_block_cache_ = true;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, write_size - 100, handle);
  ASSERT_EQ(OB_ITER_END, ret);
  ASSERT_EQ(100, handle.get_done_size());
  cmp = memcmp(handle.get_buffer(), write_buf + write_size - 100, 100);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;

  /************** test read **************/
  // 1. read aligned data
  read_buf = new char [3 * ObTmpFileGlobal::PAGE_SIZE];
  io_info.buf_ = read_buf;
  io_info.size_ = 3 * ObTmpFileGlobal::PAGE_SIZE;
  ret = MTL(ObTenantTmpFileManager *)->read(io_info, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(io_info.size_, handle.get_done_size());
  cmp = memcmp(handle.get_buffer(), write_buf, io_info.size_);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;
  // 2. read unaligned data
  read_buf = new char [ObTmpFileGlobal::PAGE_SIZE];
  io_info.buf_ = read_buf;
  io_info.size_ = 100;
  ret = MTL(ObTenantTmpFileManager *)->read(io_info, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(io_info.size_, handle.get_done_size());
  cmp = memcmp(handle.get_buffer(), write_buf + 3 * ObTmpFileGlobal::PAGE_SIZE, io_info.size_);
  ASSERT_EQ(0, cmp);
  handle.reset();

  io_info.buf_ = read_buf + 100;
  io_info.size_ = ObTmpFileGlobal::PAGE_SIZE - 100;
  ret = MTL(ObTenantTmpFileManager *)->read(io_info, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(io_info.size_, handle.get_done_size());
  cmp = memcmp(handle.get_buffer(), write_buf + 3 * ObTmpFileGlobal::PAGE_SIZE + 100, io_info.size_);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;
  read_time = ObTimeUtility::current_time() - read_time;

  ret = MTL(ObTenantTmpFileManager *)->remove(fd);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(0, MTL(ObTenantTmpFileManager *)->page_cache_controller_.flush_priority_mgr_.get_file_size());
  ASSERT_EQ(0, MTL(ObTenantTmpFileManager *)->page_cache_controller_.evict_mgr_.get_file_size());

  LOG_INFO("test_read");
  LOG_INFO("io time", K(write_time), K(read_time));
}

// generate 8MB random data
// this test will check whether kv_cache caches correct pages in disk
TEST_F(TestTmpFile, test_cached_read)
{
  int ret = OB_SUCCESS;
  const int64_t write_size = 8 * 1024 * 1024; // 8MB
  const int64_t wbp_mem_limit = MTL(ObTenantTmpFileManager *)->page_cache_controller_.write_buffer_pool_.get_memory_limit();
  ASSERT_GT(write_size, wbp_mem_limit);
  char *write_buf = new char [write_size];
  for (int64_t i = 0; i < write_size;) {
    int64_t random_length = generate_random_int(1024, 8 * 1024);
    int64_t random_int = generate_random_int(0, 256);
    for (int64_t j = 0; j < random_length && i + j < write_size; ++j) {
      write_buf[i + j] = random_int;
    }
    i += random_length;
  }

  int64_t dir = -1;
  int64_t fd = -1;
  ret = MTL(ObTenantTmpFileManager *)->alloc_dir(dir);
  ASSERT_EQ(OB_SUCCESS, ret);
  ret = MTL(ObTenantTmpFileManager *)->open(fd, dir);
  std::cout << "open temporary file: " << fd << std::endl;
  ASSERT_EQ(OB_SUCCESS, ret);
  ObTmpFileHandle file_handle;
  ret = MTL(ObTenantTmpFileManager *)->get_tmp_file(fd, file_handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  file_handle.get()->page_idx_cache_.max_bucket_array_capacity_ = SMALL_WBP_IDX_CACHE_MAX_CAPACITY;

  ObTmpFileIOInfo io_info;
  io_info.fd_ = fd;
  io_info.io_desc_.set_wait_event(2);
  io_info.buf_ = write_buf;
  io_info.size_ = write_size;
  io_info.io_timeout_ms_ = DEFAULT_IO_WAIT_TIME_MS;

  // 1. Write data and wait flushing over
  int64_t write_time = ObTimeUtility::current_time();
  ret = MTL(ObTenantTmpFileManager *)->write(io_info);
  write_time = ObTimeUtility::current_time() - write_time;
  ASSERT_EQ(OB_SUCCESS, ret);
  sleep(2);

  int64_t wbp_begin_offset = file_handle.get()->cal_wbp_begin_offset();
  ASSERT_GT(wbp_begin_offset, 0);
  ASSERT_EQ(wbp_begin_offset % ObTmpFileGlobal::PAGE_SIZE, 0);

  // 2. check block kv cache
  common::ObArray<ObSharedNothingTmpFileDataItem> data_items;
  ret = file_handle.get()->meta_tree_.search_data_items(0,wbp_begin_offset, data_items);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(false, data_items.empty());
  for (int64_t i = 0; OB_SUCC(ret) && i < data_items.count(); i++) {
    const int64_t block_index = data_items[i].block_index_;
    ObTmpBlockValueHandle block_value_handle;
    ret = ObTmpBlockCache::get_instance().get_block(ObTmpBlockCacheKey(block_index, MTL_ID()),
                                                    block_value_handle);
    ASSERT_EQ(OB_SUCCESS, ret);
  }

  // 3. read data from block kv cache
  int64_t read_size = write_size;
  int64_t read_offset = 0;
  char *read_buf = new char [read_size];
  ObTmpFileIOHandle handle;
  io_info.buf_ = read_buf;
  io_info.size_ = read_size;
  io_info.disable_page_cache_ = true;
  io_info.disable_block_cache_ = false;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(io_info.size_, handle.get_done_size());
  int cmp = memcmp(handle.get_buffer(), write_buf + read_offset, io_info.size_);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;

  // 4. read disk data and puts them into kv_cache
  int64_t read_time = ObTimeUtility::current_time();
  read_size = wbp_begin_offset - ObTmpFileGlobal::PAGE_SIZE;
  read_offset = ObTmpFileGlobal::PAGE_SIZE / 2;
  read_buf = new char [read_size];
  io_info.buf_ = read_buf;
  io_info.size_ = read_size;
  io_info.disable_page_cache_ = false;
  io_info.disable_block_cache_ = true;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(io_info.size_, handle.get_done_size());
  cmp = memcmp(handle.get_buffer(), write_buf + read_offset, io_info.size_);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;

  // 6. read disk data to check whether kv_cache caches correct pages
  read_size = wbp_begin_offset;
  read_offset = 0;
  read_buf = new char [read_size];
  io_info.buf_ = read_buf;
  io_info.size_ = read_size;
  io_info.disable_page_cache_ = false;
  io_info.disable_block_cache_ = true;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(io_info.size_, handle.get_done_size());
  cmp = memcmp(handle.get_buffer(), write_buf + read_offset, io_info.size_);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;
  read_time = ObTimeUtility::current_time() - read_time;

  // 7. check pages in kv_cache
  int64_t previous_virtual_page_id = data_items.at(0).virtual_page_id_;
  for (int64_t i = 0; i < data_items.count() && OB_SUCC(ret); ++i) {
    const ObSharedNothingTmpFileDataItem &data_item = data_items.at(i);
    if (i > 0) {
      ASSERT_GT(data_item.virtual_page_id_, previous_virtual_page_id);
      previous_virtual_page_id = data_item.virtual_page_id_;
    }
    for (int64_t j = 0; j < data_item.physical_page_num_; j++) {
      int64_t physical_page_id = data_item.physical_page_id_ + j;
      ObTmpPageCacheKey key(data_item.block_index_, physical_page_id, MTL_ID());
      ObTmpPageValueHandle handle;
      ret = ObTmpPageCache::get_instance().get_page(key, handle);
      if (OB_FAIL(ret)) {
        std::cout << "get cached page failed" << i <<" "<< data_item.block_index_<<" "<< physical_page_id << std::endl;
        ob_abort();
      }
      ASSERT_EQ(OB_SUCCESS, ret);
      cmp = memcmp(handle.value_->get_buffer(), write_buf + (data_item.virtual_page_id_ + j) * ObTmpFileGlobal::PAGE_SIZE, ObTmpFileGlobal::PAGE_SIZE);
      ASSERT_EQ(0, cmp);
    }
  }

  file_handle.reset();
  ret = MTL(ObTenantTmpFileManager *)->remove(fd);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(0, MTL(ObTenantTmpFileManager *)->page_cache_controller_.flush_priority_mgr_.get_file_size());
  ASSERT_EQ(0, MTL(ObTenantTmpFileManager *)->page_cache_controller_.evict_mgr_.get_file_size());

  LOG_INFO("test_cached_read");
  LOG_INFO("io time", K(write_time), K(read_time));
}

// 1. append write a uncompleted tail page in memory
// 2. append write a uncompleted tail page in disk
TEST_F(TestTmpFile, test_write_tail_page)
{
  int ret = OB_SUCCESS;
  const int64_t write_size = 10 * 1024; // 10KB
  int64_t already_write_size = 0;
  char *write_buf = new char [write_size];
  for (int64_t i = 0; i < write_size;) {
    int64_t random_length = generate_random_int(1024, 8 * 1024);
    int64_t random_int = generate_random_int(0, 256);
    for (int64_t j = 0; j < random_length && i + j < write_size; ++j) {
      write_buf[i + j] = random_int;
    }
    i += random_length;
  }

  int64_t dir = -1;
  int64_t fd = -1;
  ret = MTL(ObTenantTmpFileManager *)->alloc_dir(dir);
  ASSERT_EQ(OB_SUCCESS, ret);
  ret = MTL(ObTenantTmpFileManager *)->open(fd, dir);
  std::cout << "open temporary file: " << fd << std::endl;
  ASSERT_EQ(OB_SUCCESS, ret);
  ObTmpFileHandle file_handle;
  ret = MTL(ObTenantTmpFileManager *)->get_tmp_file(fd, file_handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  file_handle.get()->page_idx_cache_.max_bucket_array_capacity_ = SMALL_WBP_IDX_CACHE_MAX_CAPACITY;
  file_handle.reset();

  // 1. write 2KB data and check rightness of writing
  ObTmpFileIOInfo io_info;
  io_info.fd_ = fd;
  io_info.io_desc_.set_wait_event(2);
  io_info.buf_ = write_buf;
  io_info.size_ = 2 * 1024; // 2KB
  io_info.io_timeout_ms_ = DEFAULT_IO_WAIT_TIME_MS;
  io_info.disable_block_cache_ = true;
  ret = MTL(ObTenantTmpFileManager *)->write(io_info);
  ASSERT_EQ(OB_SUCCESS, ret);
  already_write_size += io_info.size_;

  int64_t read_size = 2 * 1024; // 2KB
  int64_t read_offset = 0;
  char *read_buf = new char [read_size];
  ObTmpFileIOHandle handle;
  io_info.buf_ = read_buf;
  io_info.size_ = read_size;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(io_info.size_, handle.get_done_size());
  int cmp = memcmp(handle.get_buffer(), write_buf + read_offset, io_info.size_);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;

  // 2. append write 2KB data in memory and check rightness of writing
  io_info.buf_ = write_buf + 2 * 1024; // 2KB
  io_info.size_ = 2 * 1024; // 2KB
  ret = MTL(ObTenantTmpFileManager *)->write(io_info);
  ASSERT_EQ(OB_SUCCESS, ret);
  already_write_size += io_info.size_;

  read_size = 4 * 1024; // 4KB
  read_offset = 0;
  read_buf = new char [read_size];
  io_info.buf_ = read_buf;
  io_info.size_ = read_size;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(io_info.size_, handle.get_done_size());
  cmp = memcmp(handle.get_buffer(), write_buf + read_offset, io_info.size_);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;

  // 3. forcibly evict current page
  ObTmpFilePageCacheController &pc_ctrl = MTL(ObTenantTmpFileManager *)->get_page_cache_controller();
  ATOMIC_SET(&pc_ctrl.flush_all_data_, true);
  pc_ctrl.flush_tg_.notify_doing_flush();
  sleep(2);
  ret = MTL(ObTenantTmpFileManager *)->get_tmp_file(fd, file_handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ret = file_handle.get()->page_cache_controller_->invoke_swap_and_wait(write_size);
  ASSERT_EQ(OB_SUCCESS, ret);
  int64_t wbp_begin_offset = file_handle.get()->cal_wbp_begin_offset();
  ASSERT_EQ(wbp_begin_offset, already_write_size);

  // 4. read disk page and add it into kv_cache
  read_offset = 5;
  read_size = already_write_size - read_offset; // 4KB - 5B
  read_buf = new char [read_size];
  io_info.buf_ = read_buf;
  io_info.size_ = read_size;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(io_info.size_, handle.get_done_size());
  cmp = memcmp(handle.get_buffer(), write_buf + read_offset, io_info.size_);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;

  // 5. append write 6KB data in memory and check rightness of writing
  io_info.buf_ = write_buf + already_write_size;
  io_info.size_ = write_size - already_write_size; // 6KB
  ret = MTL(ObTenantTmpFileManager *)->write(io_info);
  ASSERT_EQ(OB_SUCCESS, ret);
  already_write_size += io_info.size_;

  read_size = write_size;
  read_offset = 0;
  read_buf = new char [read_size];
  io_info.buf_ = read_buf;
  io_info.size_ = read_size;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(io_info.size_, handle.get_done_size());
  cmp = memcmp(handle.get_buffer(), write_buf + read_offset, io_info.size_);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;

  // 6. forcibly evict all pages and read them from disk to check whether hit old cached page in kv_cache
  pc_ctrl.flush_tg_.notify_doing_flush();
  sleep(2);
  ret = file_handle.get()->page_cache_controller_->invoke_swap_and_wait(write_size);
  ASSERT_EQ(OB_SUCCESS, ret);
  wbp_begin_offset = file_handle.get()->cal_wbp_begin_offset();
  ASSERT_EQ(wbp_begin_offset, already_write_size);
  ATOMIC_SET(&pc_ctrl.flush_all_data_, false);

  read_offset = 20;
  read_size = write_size - read_offset; // 10KB - 20B
  read_buf = new char [read_size];
  io_info.buf_ = read_buf;
  io_info.size_ = read_size;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(io_info.size_, handle.get_done_size());
  cmp = memcmp(handle.get_buffer(), write_buf + read_offset, io_info.size_);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;

  file_handle.reset();
  ret = MTL(ObTenantTmpFileManager *)->remove(fd);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(0, MTL(ObTenantTmpFileManager *)->page_cache_controller_.flush_priority_mgr_.get_file_size());
  ASSERT_EQ(0, MTL(ObTenantTmpFileManager *)->page_cache_controller_.evict_mgr_.get_file_size());

  LOG_INFO("test_write_tail_page");
}

// 1. truncate special cases
// 2. truncate disk data (truncate_offset < wbp begin offset)
// 3. truncate memory data and disk data (wbp begin offset < truncate_offset < file_size_)
// 4. truncate() do nothing (truncate_offset < file's truncate_offset_)
// 5. invalid truncate_offset checking
TEST_F(TestTmpFile, test_tmp_file_truncate)
{
  int ret = OB_SUCCESS;
  const int64_t data_size = 30 * 1024 * 1024; // 30MB
  const int64_t wbp_mem_limit = MTL(ObTenantTmpFileManager *)->page_cache_controller_.write_buffer_pool_.get_memory_limit();
  ASSERT_GT(data_size, wbp_mem_limit);
  char *write_buf = new char [data_size];
  int64_t already_write_size = 0;
  for (int64_t i = 0; i < data_size;) {
    int64_t random_length = generate_random_int(1024, 8 * 1024);
    int64_t random_int = generate_random_int(0, 256);
    for (int64_t j = 0; j < random_length && i + j < data_size; ++j) {
      write_buf[i + j] = random_int;
    }
    i += random_length;
  }

  int64_t dir = -1;
  int64_t fd = -1;
  ret = MTL(ObTenantTmpFileManager *)->alloc_dir(dir);
  ASSERT_EQ(OB_SUCCESS, ret);
  ret = MTL(ObTenantTmpFileManager *)->open(fd, dir);
  std::cout << "open temporary file: " << fd << std::endl;
  ASSERT_EQ(OB_SUCCESS, ret);
  ObTmpFileHandle file_handle;
  ret = MTL(ObTenantTmpFileManager *)->get_tmp_file(fd, file_handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  file_handle.get()->page_idx_cache_.max_bucket_array_capacity_ = SMALL_WBP_IDX_CACHE_MAX_CAPACITY;

  // 1. truncate special cases
  // 1.1 truncate a file with several pages
  // 1.1.1 write two pages and check rightness of writing
  ObTmpFileIOInfo io_info;
  io_info.fd_ = fd;
  io_info.io_desc_.set_wait_event(2);
  io_info.io_timeout_ms_ = DEFAULT_IO_WAIT_TIME_MS;
  io_info.buf_ = write_buf;
  io_info.size_ = 2 * ObTmpFileGlobal::PAGE_SIZE;
  ret = MTL(ObTenantTmpFileManager *)->write(io_info);
  ASSERT_EQ(OB_SUCCESS, ret);
  already_write_size += io_info.size_;

  int64_t read_offset = 0;
  int64_t read_size = already_write_size;
  char *read_buf = new char [read_size];
  ObTmpFileIOHandle handle;
  io_info.buf_ = read_buf;
  io_info.disable_block_cache_ = true;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(read_size, handle.get_done_size());
  int cmp = memcmp(handle.get_buffer(), write_buf + read_offset, read_size);
  ASSERT_EQ(0, cmp);
  MEMSET(read_buf, 0, read_size);

  // 1.1.2 truncate to the middle offset of the first page
  ASSERT_EQ(file_handle.get()->cached_page_nums_, 2);
  uint32_t begin_page_id = file_handle.get()->begin_page_id_;
  uint32_t end_page_id = file_handle.get()->end_page_id_;
  int64_t truncate_offset = ObTmpFileGlobal::PAGE_SIZE / 2;
  ret = MTL(ObTenantTmpFileManager *)->truncate(fd, truncate_offset);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(file_handle.get()->begin_page_id_, begin_page_id);

  // read_offset = 0;
  // read_size = already_write_size;
  io_info.buf_ = read_buf;
  io_info.size_ = read_size;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(read_size, handle.get_done_size());
  MEMSET(write_buf, 0, truncate_offset);
  cmp = memcmp(handle.get_buffer(), write_buf + read_offset, read_size);
  ASSERT_EQ(0, cmp);
  handle.reset();
  MEMSET(read_buf, 0, read_size);

  // 1.1.3 truncate the first page
  truncate_offset = ObTmpFileGlobal::PAGE_SIZE;
  ret = MTL(ObTenantTmpFileManager *)->truncate(fd, truncate_offset);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(file_handle.get()->begin_page_id_, end_page_id);
  ASSERT_EQ(file_handle.get()->cached_page_nums_, 1);

  // read_offset = 0;
  // read_size = already_write_size;
  io_info.buf_ = read_buf;
  io_info.size_ = read_size;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(read_size, handle.get_done_size());
  MEMSET(write_buf, 0, truncate_offset);
  cmp = memcmp(handle.get_buffer(), write_buf + read_offset, read_size);
  ASSERT_EQ(0, cmp);
  handle.reset();
  MEMSET(read_buf, 0, read_size);

  // 1.1.4 truncate whole pages
  truncate_offset = already_write_size;
  ret = MTL(ObTenantTmpFileManager *)->truncate(fd, truncate_offset);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(file_handle.get()->begin_page_id_, ObTmpFileGlobal::INVALID_PAGE_ID);
  ASSERT_EQ(file_handle.get()->cached_page_nums_, 0);

  // read_offset = 0;
  // read_size = already_write_size;
  io_info.buf_ = read_buf;
  io_info.size_ = read_size;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(read_size, handle.get_done_size());
  MEMSET(write_buf, 0, truncate_offset);
  cmp = memcmp(handle.get_buffer(), write_buf + read_offset, read_size);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;

  // 1.2 truncate a offset of a page whose page index is not in index cache (to mock the sparsify case of index cache)
  // 1.2.1 write three pages and check rightness of writing
  read_offset = already_write_size;
  io_info.buf_ = write_buf + already_write_size;
  io_info.size_ = 3 * ObTmpFileGlobal::PAGE_SIZE;
  ret = MTL(ObTenantTmpFileManager *)->write(io_info);
  ASSERT_EQ(OB_SUCCESS, ret);
  already_write_size += io_info.size_;

  read_size = io_info.size_;
  read_buf = new char [read_size];
  io_info.buf_ = read_buf;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(read_size, handle.get_done_size());
  cmp = memcmp(handle.get_buffer(), write_buf + read_offset, read_size);
  ASSERT_EQ(0, cmp);
  delete[] read_buf;

  // 1.2.2 pop the first page index of index cache of file
  ASSERT_NE(file_handle.get()->page_idx_cache_.page_buckets_, nullptr);
  ASSERT_EQ(file_handle.get()->page_idx_cache_.size(), 1);
  ObTmpFileWBPIndexCache::ObTmpFilePageIndexBucket *bucket = file_handle.get()->page_idx_cache_.page_buckets_->at(0);
  ASSERT_NE(bucket, nullptr);
  ASSERT_EQ(bucket->size(), 3);
  begin_page_id = file_handle.get()->begin_page_id_;
  end_page_id = file_handle.get()->end_page_id_;
  ASSERT_EQ(bucket->page_indexes_.at(bucket->left_), begin_page_id);
  ASSERT_EQ(bucket->page_indexes_.at(bucket->right_), end_page_id);
  ret = bucket->pop_();
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(bucket->size(), 2);
  ASSERT_NE(bucket->page_indexes_.at(bucket->left_), begin_page_id);
  ASSERT_EQ(bucket->page_indexes_.at(bucket->right_), end_page_id);

  // 1.2.3 truncate the first page
  ASSERT_EQ(file_handle.get()->cached_page_nums_, 3);
  truncate_offset = read_offset + ObTmpFileGlobal::PAGE_SIZE;
  ret = MTL(ObTenantTmpFileManager *)->truncate(fd, truncate_offset);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(file_handle.get()->cached_page_nums_, 2);

  read_size = already_write_size - read_offset;
  read_buf = new char [read_size];
  io_info.buf_ = read_buf;
  io_info.size_ = read_size;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(read_size, handle.get_done_size());
  MEMSET(write_buf, 0, truncate_offset);
  cmp = memcmp(handle.get_buffer(), write_buf + read_offset, read_size);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;

  // 2. truncate disk data (truncate_offset < wbp begin offset)
  read_offset = already_write_size;
  io_info.buf_ = write_buf + already_write_size;
  io_info.size_ = data_size - already_write_size;
  ret = MTL(ObTenantTmpFileManager *)->write(io_info);
  ASSERT_EQ(OB_SUCCESS, ret);
  int64_t wbp_begin_offset = file_handle.get()->cal_wbp_begin_offset();
  ASSERT_GT(wbp_begin_offset, 0);

  truncate_offset = wbp_begin_offset/2;
  read_size = wbp_begin_offset - read_offset;
  ret = MTL(ObTenantTmpFileManager *)->truncate(fd, truncate_offset);
  ASSERT_EQ(OB_SUCCESS, ret);
  read_buf = new char [read_size];
  io_info.buf_ = read_buf;
  io_info.size_ = read_size;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(read_size, handle.get_done_size());
  MEMSET(write_buf, 0, truncate_offset);
  cmp = memcmp(handle.get_buffer(), write_buf + read_offset, read_size);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;

  // 3. truncate memory data (truncate_offset < file_size_)
  // 3.1 truncate_offset is unaligned
  read_offset = truncate_offset;
  truncate_offset = (wbp_begin_offset + data_size) / 2 - ObTmpFileGlobal::PAGE_SIZE / 2;
  read_size = data_size - read_offset;
  ret = MTL(ObTenantTmpFileManager *)->truncate(fd, truncate_offset);
  ASSERT_EQ(OB_SUCCESS, ret);
  read_buf = new char [read_size];
  io_info.buf_ = read_buf;
  io_info.size_ = read_size;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(read_size, handle.get_done_size());
  MEMSET(write_buf, 0, truncate_offset);
  cmp = memcmp(handle.get_buffer(), write_buf + read_offset, read_size);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;

  read_offset = truncate_offset;
  truncate_offset = upper_align(truncate_offset, ObTmpFileGlobal::PAGE_SIZE) + ObTmpFileGlobal::PAGE_SIZE;
  read_size = data_size - read_offset;
  ret = MTL(ObTenantTmpFileManager *)->truncate(fd, truncate_offset);
  ASSERT_EQ(OB_SUCCESS, ret);
  read_buf = new char [read_size];
  io_info.buf_ = read_buf;
  io_info.size_ = read_size;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(read_size, handle.get_done_size());
  MEMSET(write_buf, 0, truncate_offset);
  cmp = memcmp(handle.get_buffer(), write_buf + read_offset, read_size);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;

  // 3.2 truncate_offset is aligned
  ASSERT_EQ(truncate_offset % ObTmpFileGlobal::PAGE_SIZE, 0);
  read_offset = truncate_offset;
  truncate_offset = truncate_offset + 5 * ObTmpFileGlobal::PAGE_SIZE;
  read_size = data_size - read_offset;
  ret = MTL(ObTenantTmpFileManager *)->truncate(fd, truncate_offset);
  ASSERT_EQ(OB_SUCCESS, ret);
  read_buf = new char [read_size];
  io_info.buf_ = read_buf;
  io_info.size_ = read_size;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(read_size, handle.get_done_size());
  MEMSET(write_buf, 0, truncate_offset);
  cmp = memcmp(handle.get_buffer(), write_buf + read_offset, read_size);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;

  read_offset = truncate_offset;
  truncate_offset = data_size;
  read_size = data_size - read_offset;
  ret = MTL(ObTenantTmpFileManager *)->truncate(fd, truncate_offset);
  ASSERT_EQ(OB_SUCCESS, ret);
  read_buf = new char [read_size];
  io_info.buf_ = read_buf;
  io_info.size_ = read_size;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(read_size, handle.get_done_size());
  MEMSET(write_buf, 0, truncate_offset);
  cmp = memcmp(handle.get_buffer(), write_buf + read_offset, read_size);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;

  // 4. truncate() do nothing (truncate_offset < file's truncate_offset_)
  int64_t old_truncate_offset = truncate_offset;
  ASSERT_EQ(old_truncate_offset, file_handle.get()->truncated_offset_);
  truncate_offset = wbp_begin_offset;
  ret = MTL(ObTenantTmpFileManager *)->truncate(fd, truncate_offset);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(old_truncate_offset, file_handle.get()->truncated_offset_);

  // 5. invalid truncate_offset checking
  ret = MTL(ObTenantTmpFileManager *)->truncate(fd, -1);
  ASSERT_NE(OB_SUCCESS, ret);
  ret = MTL(ObTenantTmpFileManager *)->truncate(fd, data_size + 10);
  ASSERT_NE(OB_SUCCESS, ret);

  file_handle.reset();
  ret = MTL(ObTenantTmpFileManager *)->remove(fd);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(0, MTL(ObTenantTmpFileManager *)->page_cache_controller_.flush_priority_mgr_.get_file_size());
  ASSERT_EQ(0, MTL(ObTenantTmpFileManager *)->page_cache_controller_.evict_mgr_.get_file_size());

  LOG_INFO("test_tmp_file_truncate");
}

// generate 750MB random data.
// this test will trigger flush and evict logic for both data and meta pages.
void test_big_file(const int64_t write_size, const int64_t wbp_mem_limit, ObTmpFileIOInfo io_info)
{
  int ret = OB_SUCCESS;
  ASSERT_GT(write_size, wbp_mem_limit);
  MTL(ObTenantTmpFileManager *)->page_cache_controller_.write_buffer_pool_.default_wbp_memory_limit_ = wbp_mem_limit;
  const int64_t macro_block_size = OB_SERVER_BLOCK_MGR.get_macro_block_size();
  int cmp = 0;
  char *write_buf = (char *)malloc(write_size);
  for (int64_t i = 0; i < write_size;) {
    int64_t random_length = generate_random_int(1024, 8 * 1024);
    int64_t random_int = generate_random_int(0, 256);
    for (int64_t j = 0; j < random_length && i + j < write_size; ++j) {
      write_buf[i + j] = random_int;
    }
    i += random_length;
  }

  int64_t dir = -1;
  int64_t fd = -1;
  ret = MTL(ObTenantTmpFileManager *)->alloc_dir(dir);
  ASSERT_EQ(OB_SUCCESS, ret);
  ret = MTL(ObTenantTmpFileManager *)->open(fd, dir);
  std::cout << "open temporary file: " << fd << " tenant_id:"<< MTL_ID() << std::endl;
  ASSERT_EQ(OB_SUCCESS, ret);
  ObTmpFileHandle file_handle;
  ret = MTL(ObTenantTmpFileManager *)->get_tmp_file(fd, file_handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  file_handle.get()->page_idx_cache_.max_bucket_array_capacity_ = SMALL_WBP_IDX_CACHE_MAX_CAPACITY;
  file_handle.reset();
  io_info.fd_ = fd;
  io_info.io_desc_.set_wait_event(2);
  io_info.io_timeout_ms_ = DEFAULT_IO_WAIT_TIME_MS;

  // 1. write 750MB data
  io_info.buf_ = write_buf;
  io_info.size_ = write_size;
  int64_t write_time = ObTimeUtility::current_time();
  ret = MTL(ObTenantTmpFileManager *)->write(io_info);
  ASSERT_EQ(OB_SUCCESS, ret);
  write_time = ObTimeUtility::current_time() - write_time;

  // 2. read 750MB data
  ObTmpFileIOHandle handle;
  int64_t read_size = write_size;
  char *read_buf = new char [read_size];
  io_info.buf_ = read_buf;
  io_info.size_ = read_size;
  ret = MTL(ObTenantTmpFileManager *)->read(io_info, handle);
  cmp = memcmp(handle.get_buffer(), write_buf, handle.get_done_size());
  ASSERT_EQ(read_size, handle.get_done_size());
  handle.reset();
  ASSERT_EQ(0, cmp);
  memset(read_buf, 0, read_size);

  // 3. attempt to read data when reach the end of file
  int64_t read_time = ObTimeUtility::current_time();
  io_info.size_ = 10;
  ret = MTL(ObTenantTmpFileManager *)->read(io_info, handle);
  ASSERT_EQ(OB_ITER_END, ret);
  handle.reset();

  // 4. pread 2MB
  int64_t read_offset = 100;
  read_size = macro_block_size;
  io_info.size_ = read_size;
  ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(read_size, handle.get_done_size());
  cmp = memcmp(handle.get_buffer(), write_buf + read_offset, handle.get_done_size());
  handle.reset();
  ASSERT_EQ(0, cmp);
  memset(read_buf + read_offset, 0, read_size);

  // 5. attempt to read data when reach the end of file (after pread)
  io_info.size_ = 10;
  ret = MTL(ObTenantTmpFileManager *)->read(io_info, handle);
  ASSERT_EQ(OB_ITER_END, ret);
  handle.reset();

  // 6. pread data which has been read to use kv_cache
  int loop_count = 30;
  for (int i = 0; i < loop_count; ++i) {
    read_offset = macro_block_size * (40 + i);
    read_size = macro_block_size * 2;
    io_info.size_ = read_size;
    ret = MTL(ObTenantTmpFileManager *)->pread(io_info, read_offset, handle);
    ASSERT_EQ(OB_SUCCESS, ret);
    ASSERT_EQ(read_size, handle.get_done_size());
    cmp = memcmp(handle.get_buffer(), write_buf + read_offset, handle.get_done_size());
    handle.reset();
    ASSERT_EQ(0, cmp);
    memset(read_buf + read_offset, 0, read_size);
  }
  read_time = ObTimeUtility::current_time() - read_time;

  free(write_buf);
  free(read_buf);

  ret = MTL(ObTenantTmpFileManager *)->remove(fd);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(0, MTL(ObTenantTmpFileManager *)->page_cache_controller_.flush_priority_mgr_.get_file_size());
  ASSERT_EQ(0, MTL(ObTenantTmpFileManager *)->page_cache_controller_.evict_mgr_.get_file_size());

  STORAGE_LOG(INFO, "test_big_file", K(io_info.disable_page_cache_), K(io_info.disable_block_cache_));
  STORAGE_LOG(INFO, "io time", K(write_time), K(read_time));
}

TEST_F(TestTmpFile, test_big_file_with_small_wbp)
{
  const int64_t write_size = 150 * 1024 * 1024;  // write 150MB data
  const int64_t wbp_mem_limit = SMALL_WBP_MEM_LIMIT;
  ObTmpFileIOInfo io_info;
  io_info.disable_page_cache_ = true;
  io_info.disable_block_cache_ = true;
  test_big_file(write_size, wbp_mem_limit, io_info);
}

// generate 16MB random data for four files. (total 64MB)
// 1. the first three files write and read 1020KB data (will not trigger flushing)
// 2. the 4th file writes and reads 3MB+1020KB data (will trigger flushing in the processing of writing)
// 3. the first three files write and read 1MB data 3 times (total 3MB)
// 4. each file read and write 12MB+4KB data
void test_multi_file_single_thread_read_write(bool disable_block_cache)
{
  int ret = OB_SUCCESS;
  const int64_t buf_size = 64 * 1024 * 1024; // 64MB
  const int64_t wbp_mem_limit = MTL(ObTenantTmpFileManager *)->page_cache_controller_.write_buffer_pool_.get_memory_limit();
  ASSERT_GT(buf_size, wbp_mem_limit);
  char *random_buf = new char [buf_size];
  for (int64_t i = 0; i < buf_size;) {
    int64_t random_length = generate_random_int(1024, 8 * 1024);
    int64_t random_int = generate_random_int(0, 256);
    for (int64_t j = 0; j < random_length && i + j < buf_size; ++j) {
      random_buf[i + j] = random_int;
    }
    i += random_length;
  }

  int64_t dir1 = -1;
  int64_t dir2 = -1;
  ret = MTL(ObTenantTmpFileManager *)->alloc_dir(dir1);
  ASSERT_EQ(OB_SUCCESS, ret);
  ret = MTL(ObTenantTmpFileManager *)->alloc_dir(dir2);
  ASSERT_EQ(OB_SUCCESS, ret);
  const int64_t file_num = 4;
  char *write_bufs[file_num] = {nullptr};
  int64_t already_write_sizes[file_num] = {0};
  int64_t fds[file_num] = {-1};
  for (int i = 0; i < file_num; ++i) {
    int64_t dir = i % 2 == 0 ? dir1 : dir2;
    int64_t fd = -1;
    ret = MTL(ObTenantTmpFileManager *)->open(fd, dir);
    std::cout << "open temporary file: " << fd << std::endl;
    ASSERT_EQ(OB_SUCCESS, ret);
    fds[i] = fd;
    write_bufs[i] = random_buf + i * buf_size / file_num;
    ObTmpFileHandle file_handle;
    ret = MTL(ObTenantTmpFileManager *)->get_tmp_file(fd, file_handle);
    ASSERT_EQ(OB_SUCCESS, ret);
    file_handle.get()->page_idx_cache_.max_bucket_array_capacity_ = SMALL_WBP_IDX_CACHE_MAX_CAPACITY;
    file_handle.reset();
  }
  ObTmpFileIOInfo io_info;
  io_info.io_desc_.set_wait_event(2);
  io_info.io_timeout_ms_ = DEFAULT_IO_WAIT_TIME_MS;
  io_info.disable_block_cache_ = disable_block_cache;
  ObTmpFileIOHandle handle;
  int cmp = 0;

  // 1. the first three files write and read 1020KB data (will not trigger flushing)
  int64_t write_size = 1020;
  io_info.size_ = write_size;
  for (int i = 0; OB_SUCC(ret) && i < file_num - 1; i++) {
    io_info.fd_ = fds[i];
    io_info.buf_ = write_bufs[i] + already_write_sizes[i];
    ret = MTL(ObTenantTmpFileManager *)->write(io_info);
    ASSERT_EQ(OB_SUCCESS, ret);
  }

  char *read_buf = new char [write_size];
  io_info.buf_ = read_buf;
  io_info.size_ = write_size;
  for (int i = 0; OB_SUCC(ret) && i < file_num - 1; i++) {
    io_info.fd_ = fds[i];
    ret = MTL(ObTenantTmpFileManager *)->read(io_info, handle);
    ASSERT_EQ(OB_SUCCESS, ret);
    ASSERT_EQ(io_info.size_, handle.get_done_size());
    cmp = memcmp(handle.get_buffer(), write_bufs[i] + already_write_sizes[i], io_info.size_);
    ASSERT_EQ(0, cmp);
    handle.reset();
    memset(read_buf, 0, write_size);
  }
  delete[] read_buf;

  for (int i = 0; OB_SUCC(ret) && i < file_num - 1; i++) {
    already_write_sizes[i] += write_size;
  }
  // 2. the 4th file writes and reads 3MB+1020KB data (will trigger flushing in the processing of writing)
  write_size = 1020 + 3 * 1024 * 1024;
  io_info.size_ = write_size;
  io_info.fd_ = fds[file_num - 1];
  io_info.buf_ = write_bufs[file_num - 1] + already_write_sizes[file_num - 1];
  ret = MTL(ObTenantTmpFileManager *)->write(io_info);
  ASSERT_EQ(OB_SUCCESS, ret);

  read_buf = new char [write_size];
  io_info.buf_ = read_buf;
  io_info.size_ = write_size;
  io_info.fd_ = fds[file_num - 1];
  ret = MTL(ObTenantTmpFileManager *)->read(io_info, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(io_info.size_, handle.get_done_size());
  cmp = memcmp(handle.get_buffer(), write_bufs[file_num - 1] + already_write_sizes[file_num - 1], io_info.size_);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;
  already_write_sizes[file_num - 1] += write_size;

  // 3. the first three files write and read 1MB data 3 times
  write_size = 1024 * 1024;
  io_info.size_ = write_size;
  const int loop_cnt = 3;
  read_buf = new char [write_size];
  for (int cnt = 0; OB_SUCC(ret) && cnt < loop_cnt; cnt++) {
    for (int i = 0; OB_SUCC(ret) && i < file_num - 1; i++) {
      io_info.fd_ = fds[i];
      io_info.buf_ = write_bufs[i] + already_write_sizes[i];
      ret = MTL(ObTenantTmpFileManager *)->write(io_info);
      ASSERT_EQ(OB_SUCCESS, ret);
    }

    io_info.buf_ = read_buf;
    io_info.size_ = write_size;
    for (int i = 0; OB_SUCC(ret) && i < file_num - 1; i++) {
      io_info.fd_ = fds[i];
      ret = MTL(ObTenantTmpFileManager *)->read(io_info, handle);
      ASSERT_EQ(OB_SUCCESS, ret);
      ASSERT_EQ(io_info.size_, handle.get_done_size());
      cmp = memcmp(handle.get_buffer(), write_bufs[i] + already_write_sizes[i], io_info.size_);
      ASSERT_EQ(0, cmp);
      handle.reset();
      memset(read_buf, 0, write_size);
    }
    for (int i = 0; OB_SUCC(ret) && i < file_num - 1; i++) {
      already_write_sizes[i] += write_size;
    }
  }
  delete[] read_buf;
  // 4. each file read and write 12MB+4KB data
  write_size = 12 * 1024 * 1024 + 4 * 1024;
  io_info.size_ = write_size;
  read_buf = new char [write_size];
  for (int i = 0; OB_SUCC(ret) && i < file_num; i++) {
    io_info.fd_ = fds[i];
    io_info.buf_ = write_bufs[i] + already_write_sizes[i];
    ret = MTL(ObTenantTmpFileManager *)->write(io_info);
    ASSERT_EQ(OB_SUCCESS, ret);
  }

  io_info.buf_ = read_buf;
  io_info.size_ = write_size;
  for (int i = 0; OB_SUCC(ret) && i < file_num; i++) {
    io_info.fd_ = fds[i];
    ret = MTL(ObTenantTmpFileManager *)->read(io_info, handle);
    ASSERT_EQ(OB_SUCCESS, ret);
    ASSERT_EQ(io_info.size_, handle.get_done_size());
    cmp = memcmp(handle.get_buffer(), write_bufs[i] + already_write_sizes[i], io_info.size_);
    ASSERT_EQ(0, cmp);
    handle.reset();
    memset(read_buf, 0, write_size);
  }
  delete[] read_buf;
  for (int i = 0; OB_SUCC(ret) && i < file_num; i++) {
    already_write_sizes[i] += write_size;
  }

  for (int i = 0; OB_SUCC(ret) && i < file_num; i++) {
    ret = MTL(ObTenantTmpFileManager *)->remove(fds[i]);
    ASSERT_EQ(OB_SUCCESS, ret);
  }
  ASSERT_EQ(0, MTL(ObTenantTmpFileManager *)->page_cache_controller_.flush_priority_mgr_.get_file_size());
  ASSERT_EQ(0, MTL(ObTenantTmpFileManager *)->page_cache_controller_.evict_mgr_.get_file_size());

  LOG_INFO("test_multi_file_single_thread_read_write", K(disable_block_cache));
}

TEST_F(TestTmpFile, test_multi_file_single_thread_read_write)
{
  test_multi_file_single_thread_read_write(false);
}

TEST_F(TestTmpFile, test_multi_file_single_thread_read_write_with_disable_block_cache)
{
  test_multi_file_single_thread_read_write(true);
}

TEST_F(TestTmpFile, test_single_file_multi_thread_read_write)
{
  int ret = OB_SUCCESS;
  const int64_t read_thread_cnt = 4;
  const int64_t file_cnt = 1;
  const int64_t batch_size = 64 * 1024 * 1024; // 64MB
  const int64_t batch_num = 4;
  const bool disable_block_cache = true;
  TestMultiTmpFileStress test(MTL_CTX());
  int64_t dir = -1;
  ret = MTL(ObTenantTmpFileManager *)->alloc_dir(dir);
  ASSERT_EQ(OB_SUCCESS, ret);
  ret = test.init(file_cnt, dir, read_thread_cnt, batch_size, batch_num, disable_block_cache);
  ASSERT_EQ(OB_SUCCESS, ret);
  int64_t io_time = ObTimeUtility::current_time();
  test.start();
  test.wait();
  io_time = ObTimeUtility::current_time() - io_time;

  STORAGE_LOG(INFO, "test_single_file_multi_thread_read_write");
  STORAGE_LOG(INFO, "io time", K(io_time));
}

TEST_F(TestTmpFile, test_multi_file_multi_thread_read_write)
{
  int ret = OB_SUCCESS;
  MTL(ObTenantTmpFileManager *)->page_cache_controller_.write_buffer_pool_.set_max_data_page_usage_ratio_(0.99);
  const int64_t read_thread_cnt = 4;
  const int64_t file_cnt = 4;
  const int64_t batch_size = 16 * 1024 * 1024; // 16MB
  const int64_t batch_num = 4;
  const bool disable_block_cache = true;
  TestMultiTmpFileStress test(MTL_CTX());
  int64_t dir = -1;
  ret = MTL(ObTenantTmpFileManager *)->alloc_dir(dir);
  ASSERT_EQ(OB_SUCCESS, ret);
  ret = test.init(file_cnt, dir, read_thread_cnt, batch_size, batch_num, disable_block_cache);
  ASSERT_EQ(OB_SUCCESS, ret);
  int64_t io_time = ObTimeUtility::current_time();
  test.start();
  test.wait();
  io_time = ObTimeUtility::current_time() - io_time;
  MTL(ObTenantTmpFileManager *)->page_cache_controller_.write_buffer_pool_.set_max_data_page_usage_ratio_(0.90);
  STORAGE_LOG(INFO, "test_multi_file_multi_thread_read_write");
  STORAGE_LOG(INFO, "io time", K(io_time));
}

TEST_F(TestTmpFile, test_multi_file_multi_thread_read_write_with_block_cache)
{
  int ret = OB_SUCCESS;
  MTL(ObTenantTmpFileManager *)->page_cache_controller_.write_buffer_pool_.set_max_data_page_usage_ratio_(0.99);
  const int64_t read_thread_cnt = 4;
  const int64_t file_cnt = 4;
  const int64_t batch_size = 16 * 1024 * 1024; // 16MB
  const int64_t batch_num = 4;
  const bool disable_block_cache = false;
  TestMultiTmpFileStress test(MTL_CTX());
  int64_t dir = -1;
  ret = MTL(ObTenantTmpFileManager *)->alloc_dir(dir);
  ASSERT_EQ(OB_SUCCESS, ret);
  ret = test.init(file_cnt, dir, read_thread_cnt, batch_size, batch_num, disable_block_cache);
  ASSERT_EQ(OB_SUCCESS, ret);
  int64_t io_time = ObTimeUtility::current_time();
  test.start();
  test.wait();
  io_time = ObTimeUtility::current_time() - io_time;
  MTL(ObTenantTmpFileManager *)->page_cache_controller_.write_buffer_pool_.set_max_data_page_usage_ratio_(0.90);
  STORAGE_LOG(INFO, "test_multi_file_multi_thread_read_write");
  STORAGE_LOG(INFO, "io time", K(io_time));
}

TEST_F(TestTmpFile, test_more_files_more_threads_read_write)
{
  int ret = OB_SUCCESS;
  const int64_t read_thread_cnt = 1;
  const int64_t file_cnt = 128;
  const int64_t batch_size = 3 * 1024 * 1024;
  const int64_t batch_num = 2; // total 128 * 3MB * 2 = 768MB
  const bool disable_block_cache = true;
  TestMultiTmpFileStress test(MTL_CTX());
  int64_t dir = -1;
  ret = MTL(ObTenantTmpFileManager *)->alloc_dir(dir);
  ASSERT_EQ(OB_SUCCESS, ret);
  ret = test.init(file_cnt, dir, read_thread_cnt, batch_size, batch_num, disable_block_cache);
  ASSERT_EQ(OB_SUCCESS, ret);
  int64_t io_time = ObTimeUtility::current_time();
  test.start();
  test.wait();
  io_time = ObTimeUtility::current_time() - io_time;

  STORAGE_LOG(INFO, "test_more_files_more_threads_read_write");
  STORAGE_LOG(INFO, "io time", K(io_time));
}

TEST_F(TestTmpFile, test_big_file)
{
  const int64_t write_size = 750 * 1024 * 1024;  // write 750MB data
  const int64_t wbp_mem_limit = BIG_WBP_MEM_LIMIT;
  ObTmpFileIOInfo io_info;
  io_info.disable_page_cache_ = false;
  io_info.disable_block_cache_ = false;
  test_big_file(write_size, wbp_mem_limit, io_info);
}

TEST_F(TestTmpFile, test_big_file_disable_block_cache)
{
  const int64_t write_size = 750 * 1024 * 1024;  // write 750MB data
  const int64_t wbp_mem_limit = BIG_WBP_MEM_LIMIT;
  ObTmpFileIOInfo io_info;
  io_info.disable_page_cache_ = false;
  io_info.disable_block_cache_ = true;
  test_big_file(write_size, wbp_mem_limit, io_info);
}

TEST_F(TestTmpFile, test_big_file_disable_page_cache)
{
  const int64_t write_size = 750 * 1024 * 1024;  // write 750MB data
  const int64_t wbp_mem_limit = BIG_WBP_MEM_LIMIT;
  ObTmpFileIOInfo io_info;
  io_info.disable_page_cache_ = true;
  io_info.disable_block_cache_ = true;
  test_big_file(write_size, wbp_mem_limit, io_info);
}

TEST_F(TestTmpFile, test_aio_pread)
{
  int ret = OB_SUCCESS;
  const int64_t write_size = 10 * 1024 * 1024; // 10MB
  char *write_buf = new char [write_size];
  for (int64_t i = 0; i < write_size;) {
    int64_t random_length = generate_random_int(1024, 8 * 1024);
    int64_t random_int = generate_random_int(0, 256);
    for (int64_t j = 0; j < random_length && i + j < write_size; ++j) {
      write_buf[i + j] = random_int;
    }
    i += random_length;
  }

  int64_t dir = -1;
  int64_t fd = -1;
  ret = MTL(ObTenantTmpFileManager *)->alloc_dir(dir);
  ASSERT_EQ(OB_SUCCESS, ret);
  ret = MTL(ObTenantTmpFileManager *)->open(fd, dir);
  std::cout << "open temporary file: " << fd << std::endl;
  ASSERT_EQ(OB_SUCCESS, ret);
  ObTmpFileHandle file_handle;
  ret = MTL(ObTenantTmpFileManager *)->get_tmp_file(fd, file_handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  file_handle.get()->page_idx_cache_.max_bucket_array_capacity_ = SMALL_WBP_IDX_CACHE_MAX_CAPACITY;

  ObTmpFileIOInfo io_info;
  io_info.fd_ = fd;
  io_info.io_desc_.set_wait_event(2);
  io_info.buf_ = write_buf;
  io_info.size_ = write_size;
  io_info.io_timeout_ms_ = DEFAULT_IO_WAIT_TIME_MS;

  // 1. Write data
  int64_t write_time = ObTimeUtility::current_time();
  ret = MTL(ObTenantTmpFileManager *)->write(io_info);
  write_time = ObTimeUtility::current_time() - write_time;
  ASSERT_EQ(OB_SUCCESS, ret);

  // 2. check aio_pread
  int64_t read_size = 9 * 1024 * 1024; // 9MB
  int64_t read_offset = 0;
  char *read_buf = new char [read_size];
  ObTmpFileIOHandle handle;
  io_info.buf_ = read_buf;
  io_info.size_ = read_size;
  ret = MTL(ObTenantTmpFileManager *)->aio_pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(0, handle.get_done_size());
  ret = handle.wait();
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(io_info.size_, handle.get_done_size());
  int cmp = memcmp(handle.get_buffer(), write_buf + read_offset, io_info.size_);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;

  // 3. execute two aio_pread, but io_handle doesn't not call wait()
  read_size = 5 * 1024 * 1024; // 5MB
  read_offset = 0;
  read_buf = new char [read_size];
  io_info.buf_ = read_buf;
  io_info.size_ = read_size;
  ret = MTL(ObTenantTmpFileManager *)->aio_pread(io_info, read_offset, handle);
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(0, handle.get_done_size());

  int read_offset2 = read_offset + read_size;
  ret = MTL(ObTenantTmpFileManager *)->aio_pread(io_info, read_offset2, handle);
  ASSERT_NE(OB_SUCCESS, ret);

  ret = handle.wait();
  ASSERT_EQ(OB_SUCCESS, ret);
  ASSERT_EQ(io_info.size_, handle.get_done_size());
  cmp = memcmp(handle.get_buffer(), write_buf + read_offset, io_info.size_);
  ASSERT_EQ(0, cmp);
  handle.reset();
  delete[] read_buf;

  file_handle.reset();
  ret = MTL(ObTenantTmpFileManager *)->remove(fd);
  ASSERT_EQ(OB_SUCCESS, ret);

  LOG_INFO("test_cached_read");
}
} // namespace oceanbase

int main(int argc, char **argv)
{
  int ret = 0;
  system("rm -f ./test_sn_tmp_file.log*");
  system("rm -rf ./run*");
  OB_LOGGER.set_file_name("test_sn_tmp_file.log", true);
  OB_LOGGER.set_log_level("INFO");
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}