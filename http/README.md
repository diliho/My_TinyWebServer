## 修复一个bug
### 缓冲区溢出风险
strcpy会一直复制直到遇到\0，若输入的user、passwd、sqlname字符串长度超过 99 字节（含\0则为 100 字节），会越界写入相邻内存，导致：
覆盖其他成员变量（如m_read_buf或m_write_buf的起始部分）；
触发内存 corruption，可能导致程序崩溃或被恶意利用（如缓冲区溢出攻击
### 优化方案：增加长度校验 + 合理设置缓冲区
新增专属错误状态USER_INPUT_ERROR
do_request检测输入的user和name长度 如超出则返回新状态码
process_read()检测到USER_INPUT_ERROR 设置响应报文返回log_out_Error.html/register_out_Error.html
