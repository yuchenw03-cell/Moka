# 收集 src 下全部源码；srs/ 为纯 C99（.c），其余可为 .cpp
file(GLOB_RECURSE SRCS "src/*.c" "src/*.cpp")

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "src" "src/srs" "src/hal" "src/app" "src/ui"
    REQUIRES epdiy driver fatfs sdmmc esp_timer nvs_flash esp_partition esp_driver_gpio esp_driver_i2c esp_driver_ledc
)

# 中文注释规范：所有硬件魔术数集中在 hal/ 各 .h 头文件的宏定义中（见阶段 3）
