# Base Kernels

这里是公开给参赛者阅读和本地调试的基础算子源码：

- `kernels_impl.cpp`：真实计算实现
- `kernels_public.cpp`：公开导出符号封装
- `kernels_internal.h`：内部实现声明

说明：

- 这些源码可以公开提供，方便参赛者理解算子语义、数据布局和 baseline 行为
- 参赛者可以在本地基于这些源码做实验或调试
- 但正式评测时，系统会强制链接赛方官方版本的 judge 算子库
- 参赛者提交物不得重定义、替换或绕过官方 `cholesky`、`trsm`、`madd`
