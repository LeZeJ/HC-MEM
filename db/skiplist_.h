#ifndef STORAGE_LEVELDB_DB_SKIPLIST__H_
#define STORAGE_LEVELDB_DB_SKIPLIST__H_
#include <stdio.h>
#include <assert.h>
#include <time.h>
#include <iostream>
#include "db/log_writer.h"
#include "db/write_batch_internal.h"

namespace leveldb {

//最大层级并不是越大越好，越大查找越快但是占用内存也越高
static const int stand_max_level = 24;


//因为跳表内部有序，需要判断大小，自定义的键类型K必须重载 == 和 》操作符
class Skiplist_
{
public:
	Skiplist_()
		: current_level(0)
		, max_level(0)
		, node_count(0)
		, header(NULL)
	{
		init(stand_max_level);
	}

	~Skiplist_()
	{
		release();
	}

	bool get(const std::string &k, std::string &v, std::string new_value="~!@#$%^&*", log::Writer *log_=NULL)
	{
		node_ptr prev[stand_max_level] = {0};
		node_ptr p = find_node(k, prev);
		if (p != NULL)
		{
			if (new_value != "~!@#$%^&*")
			{
				if (log_ != NULL)
				{
					WriteBatch updates;
					updates.Put(Slice(k), Slice(new_value));
					log_->AddRecord(WriteBatchInternal::Contents(&updates));
				}
				p->value = new_value;
			}
			v = p->value;
			return true;
		}
		return false;
	}

	void set(const std::string &k, const std::string &v)
	{
		//先查找是否存在，存在则更新
		node_ptr prev[stand_max_level] = { 0 };
		node_ptr p = find_node(k, prev);
		if (p != NULL)
		{
			p->value = v;
			return;
		}
		//不存在，插入到跳表中
		int new_level = random_level();
		node_ptr new_node = init_node(new_level, k, v);
		node_count++;
		for (int i = 0; i < current_level && i < new_level; ++i)
		{
			new_node->set_level(i, prev[i]->get_level(i));
			prev[i]->set_level(i, new_node);
		}
		//高度更新后，需要连接到头节点
		if (new_level > current_level)
		{
			for (int i = current_level; i < new_level; ++i)
				header->set_level(i, new_node);
			current_level = new_level;
		}
	}

	bool remove(const std::string &k)
	{
		assert(header != NULL);
		node_ptr p = header;
		for (int i = current_level - 1; i >= 0; --i)
		{
			node_ptr next = NULL;
			while ((next = p->get_level(i)) != NULL)
			{
				if (next->key == k)
				{
					//查找到了，更新链表指针
					p->set_level(i, next->get_level(i));
					if (i == 0)
					{//ɾ���ڵ�
						delete next;
						node_count--;
						return true;
					}
					//这一层只有该元素，则更新跳表高度
					if (p == header && p->get_level(i) == NULL)
					{
						current_level--;
					}
					//继续删除下一层节点
					break;
				}
				//这一层没有这个节点，前往下一层查找
				if (next->key > k)
					break;
				//继续往前移动查找
				p = next;
			}
		}
		return false;
	}

	void print()
	{
		assert(header != NULL);
		std::cout << "\r\nSkiplist header: " << current_level << std::endl;
		for (int i = current_level-1; i >=0; --i)
		{
			std::cout << "Print line " << i + 1 <<": ";
			node_ptr p = header;
			node_ptr next = NULL;
			while ((next = p->get_level(i)) != NULL)
			{
				std::cout << next->key << "[" << next->value << "]  ";
				p = next;
			}
			std::cout << " " << std::endl;
		}
		std::cout << std::endl;
	}

	typedef unsigned char byte;
	typedef struct node {
		std::string key;
		std::string value;
		int level;
		byte* levels_buf;//节点指针的数组的首地址
		node* get_level(int index)
		{
			if (index < 0 || index >= level)
				return NULL;
			byte* dest = levels_buf + index * sizeof(node*);
			node* p = NULL;
			//将保存的地址拷贝到指针的内存中
			memcpy(&p, dest, sizeof(node*));
			return p;
		}
		bool set_level(int index, node* p_node)
		{
			if (index < 0 || index >= level)
				return false;
			byte* dest = levels_buf + index * sizeof(node*);
			unsigned long addr = (unsigned long)(void*)p_node;
			//将指针地址保存到缓存中
			memcpy(dest, &addr, sizeof(node*));
			return true;
		}

	}*node_ptr;

	node_ptr init_node(int level, const std::string &k, const std::string &v)
	{
		node_ptr p = new node;
		if (p != NULL)
		{
			p->key = k;
			p->value = v;
			int mem_size = level * sizeof(node_ptr);
			p->levels_buf = (byte*)malloc(mem_size);
			memset(p->levels_buf, 0, mem_size);
			p->level = level;
		}
		return p;
	}
	node_ptr find_node(const std::string &k, node_ptr* prev)
	{
		assert(header != NULL);
		if (node_count < 1)
			return NULL;
		node_ptr p = header;
		int setp = 0;
		for (int i = current_level - 1; i >= 0; --i)
		{
			setp++;
			node_ptr next = NULL;
			while ( (next = p->get_level(i)) != NULL)
			{
				if (next->key == k)
				{
					// std::cout << "Find key: " << k << ", step is: " << setp << std::endl;
					return next;
				}
				if (next->key > k)
				{//此层级中不存在该key，到下一层中去查找
					break;
				}
				setp++;
				p = next;
			}
			//记录下该节点
			prev[i] = p;
		}
		//std::cout << "Cannot find key: " << k << ", step is: " << setp << std::endl;
		return NULL;
	}
	bool init(int max_level)
	{
		if (header != NULL)
		{
			//re init
			return false;
		}
		this->max_level = max_level;
		srand((unsigned int)time(NULL));
		header = init_node(max_level, {}, {});
		return header != NULL;
	}
	//返回随机数--层数
	int random_level()
	{
		int level = 1;
		while (((rand() % 4) == 0) && level < max_level)
		{
			level += 1;
		}
		return level;
	}
	//释放内存空间
	void release()
	{
		if (header == NULL)
			return;
		//释放子节点
		node_ptr p = header;
		while (p != NULL)
		{
			node_ptr next = p->get_level(0);
			free(p->levels_buf);
			delete p;
			p = next;
		}
	}

	int current_level;//跳表当前层数
	int max_level;//跳表最大层数
	int node_count;//元素个数
	node_ptr header;//跳表表头
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_SKIPLIST__H_