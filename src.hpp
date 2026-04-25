
#include "fstream.h"
#include <vector>
#include <cstring>

// 磁盘事件类型：正常、故障、更换
enum class EventType {
  NORMAL,  // 正常：所有磁盘工作正常
  FAILED,  // 故障：指定磁盘发生故障（文件被删除）
  REPLACED // 更换：指定磁盘被更换（文件被清空）
};

class RAID5Controller {
private:
  std::vector<sjtu::fstream *> drives_; // 磁盘文件对应的 fstream 对象
  int blocks_per_drive_;               // 每个磁盘的块数
  int block_size_;                     // 每个块的大小
  int num_disks_;                      // 磁盘数
  std::vector<bool> drive_failed_;     // 标记磁盘是否故障

  // 计算给定块ID所在的条带
  int get_stripe_id(int block_id) const {
    return block_id / (num_disks_ - 1);
  }

  // 计算给定块ID在条带中的位置
  int get_position_in_stripe(int block_id) const {
    return block_id % (num_disks_ - 1);
  }

  // 计算给定条带的校验块所在磁盘
  int get_parity_disk(int stripe_id) const {
    return num_disks_ - 1 - (stripe_id % num_disks_);
  }

  // 计算给定块ID实际存储的磁盘
  int get_data_disk(int block_id) const {
    int stripe_id = get_stripe_id(block_id);
    int pos_in_stripe = get_position_in_stripe(block_id);
    int parity_disk = get_parity_disk(stripe_id);
    
    int data_disk = pos_in_stripe;
    if (data_disk >= parity_disk) {
      data_disk++;
    }
    return data_disk;
  }

  // 计算给定块ID在磁盘中的物理块位置
  int get_physical_block(int block_id) const {
    return get_stripe_id(block_id);
  }

  // XOR操作：计算校验块或重建数据
  void xor_blocks(char* result, const char* block1, const char* block2, int size) {
    for (int i = 0; i < size; i++) {
      result[i] = block1[i] ^ block2[i];
    }
  }

  // 读取指定磁盘的指定块
  void read_from_disk(int disk_id, int physical_block, char* buffer) {
    if (disk_id < 0 || disk_id >= num_disks_) {
      throw std::runtime_error("Invalid disk ID");
    }
    if (drive_failed_[disk_id]) {
      throw std::runtime_error("Attempting to read from failed disk");
    }
    if (physical_block < 0 || physical_block >= blocks_per_drive_) {
      throw std::runtime_error("Invalid physical block");
    }
    if (!drives_[disk_id] || !drives_[disk_id]->is_open()) {
      throw std::runtime_error("Disk file not open");
    }
    
    drives_[disk_id]->seekg(physical_block * block_size_);
    if (drives_[disk_id]->fail()) {
      throw std::runtime_error("Failed to seek in disk file");
    }
    drives_[disk_id]->read(buffer, block_size_);
    if (drives_[disk_id]->fail()) {
      throw std::runtime_error("Failed to read from disk file");
    }
  }

  // 写入指定磁盘的指定块
  void write_to_disk(int disk_id, int physical_block, const char* buffer) {
    if (disk_id < 0 || disk_id >= num_disks_) {
      throw std::runtime_error("Invalid disk ID");
    }
    if (drive_failed_[disk_id]) {
      throw std::runtime_error("Attempting to write to failed disk");
    }
    if (physical_block < 0 || physical_block >= blocks_per_drive_) {
      throw std::runtime_error("Invalid physical block");
    }
    if (!drives_[disk_id] || !drives_[disk_id]->is_open()) {
      throw std::runtime_error("Disk file not open");
    }
    
    drives_[disk_id]->seekp(physical_block * block_size_);
    if (drives_[disk_id]->fail()) {
      throw std::runtime_error("Failed to seek in disk file");
    }
    drives_[disk_id]->write(buffer, block_size_);
    if (drives_[disk_id]->fail()) {
      throw std::runtime_error("Failed to write to disk file");
    }
  }

  // 重建指定磁盘的数据
  void rebuild_drive(int drive_id) {
    if (drive_id < 0 || drive_id >= num_disks_) {
      return;
    }
    
    std::vector<char> temp_buffer(block_size_);
    std::vector<char> xor_buffer(block_size_);
    
    for (int stripe = 0; stripe < blocks_per_drive_; stripe++) {
      try {
        // 确定这个条带中，指定磁盘应该存储什么（数据还是校验）
        int parity_disk = get_parity_disk(stripe);
        
        if (drive_id == parity_disk) {
          // 这个磁盘存储校验块，需要重新计算校验
          memset(xor_buffer.data(), 0, block_size_);
          
          for (int disk = 0; disk < num_disks_; disk++) {
            if (disk != drive_id && !drive_failed_[disk]) {
              read_from_disk(disk, stripe, temp_buffer.data());
              xor_blocks(xor_buffer.data(), xor_buffer.data(), temp_buffer.data(), block_size_);
            }
          }
          
          write_to_disk(drive_id, stripe, xor_buffer.data());
        } else {
          // 这个磁盘存储数据块，需要重建数据
          // 计算这个条带中，指定磁盘存储的是第几个数据块
          int data_pos_in_stripe = -1;
          for (int pos = 0; pos < num_disks_ - 1; pos++) {
            int disk_for_pos = pos;
            if (disk_for_pos >= parity_disk) {
              disk_for_pos++;
            }
            if (disk_for_pos == drive_id) {
              data_pos_in_stripe = pos;
              break;
            }
          }
          
          if (data_pos_in_stripe >= 0) {
            // 重建数据：data = parity ^ all_other_data
            memset(xor_buffer.data(), 0, block_size_);
            bool found_parity = false;
            
            // 先找到校验块
            for (int disk = 0; disk < num_disks_; disk++) {
              if (disk != drive_id && !drive_failed_[disk] && disk == parity_disk) {
                read_from_disk(disk, stripe, temp_buffer.data());
                memcpy(xor_buffer.data(), temp_buffer.data(), block_size_);
                found_parity = true;
                break;
              }
            }
            
            // 然后XOR所有其他数据块
            for (int disk = 0; disk < num_disks_; disk++) {
              if (disk != drive_id && !drive_failed_[disk] && disk != parity_disk) {
                read_from_disk(disk, stripe, temp_buffer.data());
                xor_blocks(xor_buffer.data(), xor_buffer.data(), temp_buffer.data(), block_size_);
              }
            }
            
            if (found_parity) {
              write_to_disk(drive_id, stripe, xor_buffer.data());
            }
          }
        }
      } catch (const std::exception& e) {
        // 如果重建某个条带失败，继续下一个条带
        continue;
      }
    }
  }

public:
  RAID5Controller(std::vector<sjtu::fstream *> drives, int blocks_per_drive,
                  int block_size = 4096) {
    drives_ = drives;
    blocks_per_drive_ = blocks_per_drive;
    block_size_ = block_size;
    num_disks_ = drives.size();
    drive_failed_.resize(num_disks_, false);
  }

  /**
   * @brief 启动 RAID5 系统
   * @param event_type_ 磁盘事件类型
   * @param drive_id 发生事件的磁盘编号（如果是 NORMAL 则忽略）
   *
   * 如果是 FAILED，对应的磁盘文件会被删除。此时不可再对该文件进行读写。
   * 如果是 REPLACED，对应的磁盘文件会被清空（但文件依然存在）
   * 如果是 NORMAL，所有磁盘正常工作
   * 注：磁盘被替换之前不一定损坏。
   */
  void Start(EventType event_type_, int drive_id) {
    if (event_type_ == EventType::FAILED) {
      if (drive_id >= 0 && drive_id < num_disks_) {
        drive_failed_[drive_id] = true;
      }
    } else if (event_type_ == EventType::REPLACED) {
      if (drive_id >= 0 && drive_id < num_disks_) {
        // 先重建数据，再标记为正常
        rebuild_drive(drive_id);
        drive_failed_[drive_id] = false;
      }
    } else if (event_type_ == EventType::NORMAL) {
      // 所有磁盘正常
      for (int i = 0; i < num_disks_; i++) {
        drive_failed_[i] = false;
      }
    }
  }

  void Shutdown() {
    // 关闭所有打开的文件
    for (auto drive : drives_) {
      if (drive && drive->is_open()) {
        drive->close();
      }
    }
  }

  void ReadBlock(int block_id, char *result) {
    if (block_id < 0 || block_id >= Capacity()) {
      throw std::runtime_error("Invalid block ID");
    }
    if (!result) {
      throw std::runtime_error("Invalid result buffer");
    }

    int stripe_id = get_stripe_id(block_id);
    int data_disk = get_data_disk(block_id);
    int physical_block = get_physical_block(block_id);

    if (data_disk < 0 || data_disk >= num_disks_) {
      throw std::runtime_error("Invalid data disk calculated");
    }

    if (!drive_failed_[data_disk]) {
      // 正常读取
      read_from_disk(data_disk, physical_block, result);
    } else {
      // 磁盘故障，需要重建数据
      std::vector<char> temp_buffer(block_size_);
      std::vector<char> xor_buffer(block_size_);
      
      // 读取条带中所有正常的数据块和校验块
      int parity_disk = get_parity_disk(stripe_id);
      bool found_parity = false;
      
      // 先找到校验块
      for (int disk = 0; disk < num_disks_; disk++) {
        if (!drive_failed_[disk] && disk == parity_disk) {
          read_from_disk(disk, physical_block, temp_buffer.data());
          memcpy(xor_buffer.data(), temp_buffer.data(), block_size_);
          found_parity = true;
          break;
        }
      }
      
      // 然后XOR所有其他数据块
      for (int disk = 0; disk < num_disks_; disk++) {
        if (!drive_failed_[disk] && disk != parity_disk) {
          read_from_disk(disk, physical_block, temp_buffer.data());
          xor_blocks(xor_buffer.data(), xor_buffer.data(), temp_buffer.data(), block_size_);
        }
      }
      
      if (!found_parity) {
        throw std::runtime_error("Cannot reconstruct data: parity disk also failed");
      }
      
      memcpy(result, xor_buffer.data(), block_size_);
    }
  }

  void WriteBlock(int block_id, const char *data) {
    if (block_id < 0 || block_id >= Capacity()) {
      throw std::runtime_error("Invalid block ID");
    }
    if (!data) {
      throw std::runtime_error("Invalid data buffer");
    }

    int stripe_id = get_stripe_id(block_id);
    int data_disk = get_data_disk(block_id);
    int physical_block = get_physical_block(block_id);
    int parity_disk = get_parity_disk(stripe_id);

    if (data_disk < 0 || data_disk >= num_disks_ || parity_disk < 0 || parity_disk >= num_disks_) {
      throw std::runtime_error("Invalid disk calculated");
    }

    if (!drive_failed_[data_disk] && !drive_failed_[parity_disk]) {
      // 正常写入：读取旧数据，计算新校验，写入数据和校验
      std::vector<char> old_data(block_size_);
      std::vector<char> old_parity(block_size_);
      std::vector<char> new_parity(block_size_);

      // 读取旧数据
      read_from_disk(data_disk, physical_block, old_data.data());
      
      // 读取旧校验
      read_from_disk(parity_disk, physical_block, old_parity.data());
      
      // 计算新校验：new_parity = old_parity ^ old_data ^ new_data
      xor_blocks(new_parity.data(), old_parity.data(), old_data.data(), block_size_);
      xor_blocks(new_parity.data(), new_parity.data(), data, block_size_);
      
      // 写入新数据
      write_to_disk(data_disk, physical_block, data);
      
      // 写入新校验
      write_to_disk(parity_disk, physical_block, new_parity.data());
    } else if (drive_failed_[data_disk] && !drive_failed_[parity_disk]) {
      // 数据磁盘故障，校验磁盘正常
      // 重建旧数据用于计算新校验
      std::vector<char> old_data(block_size_);
      std::vector<char> old_parity(block_size_);
      std::vector<char> new_parity(block_size_);
      
      // 重建旧数据
      ReadBlock(block_id, old_data.data());
      
      // 读取旧校验
      read_from_disk(parity_disk, physical_block, old_parity.data());
      
      // 计算新校验：new_parity = old_parity ^ old_data ^ new_data
      xor_blocks(new_parity.data(), old_parity.data(), old_data.data(), block_size_);
      xor_blocks(new_parity.data(), new_parity.data(), data, block_size_);
      
      // 只写入新校验（数据磁盘故障，无法写入）
      write_to_disk(parity_disk, physical_block, new_parity.data());
    } else if (!drive_failed_[data_disk] && drive_failed_[parity_disk]) {
      // 校验磁盘故障，数据磁盘正常
      // 写入数据，但无法更新校验
      write_to_disk(data_disk, physical_block, data);
    } else {
      // 两个磁盘都故障了
      throw std::runtime_error("Cannot write: both data and parity disks failed");
    }
  }

  int Capacity() {
    // 返回磁盘阵列能写入的块的数量（你无需改动此函数）
    return (num_disks_ - 1) * blocks_per_drive_;
  }
};
