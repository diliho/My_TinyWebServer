#ifndef MEMPOOL_H
#define MEMPOOL_H

#include <cstddef>
#include <cstdlib>
#include <cassert>
#include <mutex>

// 通用内存池类模板
template <typename T>
class MemPool {
public:
    MemPool(size_t initial_size = 1024) {
        m_initial_size = initial_size;
        m_pool = nullptr;
        m_free_list = nullptr;
        m_pool_size = 0;
        m_allocated_count = 0;
        
        // 预分配初始内存
        allocate_pool(initial_size);
    }
    
    ~MemPool() {
        // 释放所有分配的内存块
        Node* current = m_pool;
        while (current) {
            Node* next = current->next;
            free(current);
            current = next;
        }
    }
    
    // 分配一个对象
    T* allocate() {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        if (!m_free_list) {
            // 如果没有可用的对象，分配更多内存
            allocate_pool(m_initial_size);
        }
        
        // 从空闲列表中取出一个对象
        Node* node = m_free_list;
        m_free_list = node->next;
        m_allocated_count++;
        
        // 调用构造函数初始化对象
        return new (node->data) T();
    }
    
    // 释放一个对象
    void deallocate(T* ptr) {
        if (!ptr) return;
        
        std::lock_guard<std::mutex> lock(m_mutex);
        
        // 调用析构函数
        ptr->~T();
        
        // 将对象放回空闲列表
        Node* node = reinterpret_cast<Node*>(reinterpret_cast<char*>(ptr) - offsetof(Node, data));
        node->next = m_free_list;
        m_free_list = node;
        m_allocated_count--;
    }
    
    // 获取已分配的对象数量
    size_t allocated_count() const {
        return m_allocated_count;
    }
    
private:
    // 内存池节点结构
    struct Node {
        Node* next;
        char data[sizeof(T)];
    };
    
    // 分配一个内存块
    void allocate_pool(size_t size) {
        Node* new_pool = nullptr;
        Node* tail = nullptr;
        
        // 分配size个对象的内存
        for (size_t i = 0; i < size; i++) {
            Node* node = (Node*)malloc(sizeof(Node));
            assert(node != nullptr);
            
            if (!new_pool) {
                new_pool = node;
            } else {
                tail->next = node;
            }
            tail = node;
        }
        
        // 将新分配的内存块添加到空闲列表
        if (tail) {
            tail->next = m_free_list;
            m_free_list = new_pool;
            
            // 将新分配的内存块添加到内存池链表
            tail->next = m_pool;
            m_pool = new_pool;
            
            m_pool_size += size;
        }
    }
    
    Node* m_pool;          // 内存池链表头
    Node* m_free_list;     // 空闲对象链表头
    size_t m_initial_size; // 初始分配大小
    size_t m_pool_size;    // 内存池总大小
    size_t m_allocated_count; // 当前已分配的对象数量
    std::mutex m_mutex;    // 互斥锁，保证线程安全
};

#endif // MEMPOOL_H