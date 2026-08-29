/**
 ******************************************************************************
 * @file        lvgl_demo.c
 * @version     V1.0
 * @brief       LVGL V8移植 实验
 ******************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 * 
 * 实验平台:     慧勤智远 ESP32-P4 开发板
 ******************************************************************************
 */

#include "lvgl_demo.h"
#include "lcd.h"
#include "touch.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "demos/lv_demos.h"
#include "esp_lvgl_port.h"
#include "esp_lvgl_port_disp.h"


/**
 * @brief       初始化并注册显示设备
 * @param       无
 * @retval      lvgl显示设备指针
 */
lv_disp_t *lv_port_disp_init(void)
{
    lv_disp_t *lcd_disp_handle = NULL; 

    /* 初始化显示设备LCD */
    lcd_init();                 /* LCD初始化 */

    if (lcddev.id <= 0x7084)    /* RGB屏触摸屏 */
    {
        /* 初始化LVGL显示配置 */
        const lvgl_port_display_cfg_t rgb_disp_cfg = {
            .panel_handle = lcddev.lcd_panel_handle,
            .buffer_size = lcddev.width * lcddev.width,
            .double_buffer = 0,
            .hres = lcddev.width,
            .vres = lcddev.height,
            .monochrome = false,
#if LVGL_VERSION_MAJOR >= 9
            .color_format = LV_COLOR_FORMAT_RGB565,
#endif
            .rotation = {           /* 必须与RGBLCD配置一样 */
                .swap_xy = false,
                .mirror_x = false,
                .mirror_y = false,
            },
            .flags = {
                .buff_dma = false,
                .buff_spiram = false,
                .full_refresh = false,
                .direct_mode = true,
#if LVGL_VERSION_MAJOR >= 9
                .swap_bytes = false,
#endif
            }
        };
        const lvgl_port_display_rgb_cfg_t rgb_cfg = {
            .flags = {
                .bb_mode = true,
                .avoid_tearing = true,
            }
        };

        lcd_disp_handle = lvgl_port_add_disp_rgb(&rgb_disp_cfg, &rgb_cfg);
    }
    else                        /* MIPI屏触摸屏 */
    {
        /* 初始化LVGL显示配置 */
        const lvgl_port_display_cfg_t disp_cfg = {
            .io_handle = lcddev.lcd_dbi_io,         /* 设置io_handle为lcddev.lcd_dbi_io，用于处理显示设备的IO操作 */
            .panel_handle = lcddev.lcd_panel_handle,/* 设置panel_handle为lcddev.lcd_panel_handle，用于处理显示设备的面板操作 */
            .control_handle = NULL,                 /* 设置control_handle为NULL，用于处理显示设备的控制操作 */
            .buffer_size = lcddev.width * lcddev.height,    /* 设置buffer_size为lcddev.height * lcddev.height，用于设置显示设备的缓冲区大小 */
            .double_buffer = true,                 /* 设置double_buffer为false，用于设置显示设备是否使用双缓冲 */
            .hres = lcddev.width,                   /* 设置hres为lcddev.width，用于设置显示设备的水平分辨率 */
            .vres = lcddev.height,                  /* 设置vres为lcddev.height，用于设置显示设备的垂直分辨率 */
            .monochrome = false,                    /* 设置monochrome为false，用于设置显示设备是否为单色 */
            .rotation = {                           /* 旋转值必须与esp_lcd中用于屏幕初始设置的值相同 */
                .swap_xy = false,
                .mirror_x = true,
                .mirror_y = false,
            },
#if LVGL_VERSION_MAJOR >= 9                     /* LVGL9？ */
#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
            .color_format = LV_COLOR_FORMAT_RGB888,
#else
            .color_format = LV_COLOR_FORMAT_RGB565,
#endif
#endif
            .flags = {
                .buff_dma = false,                 /* 分配的LVGL缓冲区支持DMA */
                .buff_spiram = true,               /* 分配的LVGL缓冲区使用PSRAM */
#if LVGL_VERSION_MAJOR >= 9
                .swap_bytes = (BSP_LCD_BIGENDIAN ? true : false),
#endif
                .sw_rotate = false,                 /* SW旋转不支持避免撕裂 */
                .full_refresh = false,              /* 全屏刷新 */
                .direct_mode = true,                /* 使用屏幕大小的缓冲区并绘制到绝对坐标 */
            }
        };

        const lvgl_port_display_dsi_cfg_t  dpi_cfg = {
            .flags = {
                .avoid_tearing = true,              /* 开启防撕裂 */
            }
        };

        lcd_disp_handle = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    }

    return lcd_disp_handle;                    
}

/**
 * @brief       图形库的触摸屏读取回调函数
 * @param       indev_drv   : 触摸屏设备
 * @param       data        : 输入设备数据结构体
 * @retval      无
 */
void touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    assert(indev_drv); /* 确保驱动程序有效 */
    /* 从触摸控制器读取数据到内存 */
    tp_dev.scan(0); /* 扫描触摸数据 */

    if (tp_dev.sta & TP_PRES_DOWN) /* 检查触摸是否按下 */
    {
        data->point.x = tp_dev.x[0]; /* 获取触摸点X坐标 */
        data->point.y = tp_dev.y[0]; /* 获取触摸点Y坐标 */
        data->state = LV_INDEV_STATE_PRESSED; /* 设置状态为按下 */
    }
    else
    {
        data->state = LV_INDEV_STATE_RELEASED; /* 设置状态为释放 */
    }
}

/**
 * @brief       初始化并注册输入设备
 * @param       无
 * @retval      lvgl输入设备指针
 */
lv_indev_t *lv_port_indev_init(void)
{
    /* 初始化触摸屏 */
    tp_dev.init();

    /* 初始化输入设备 */
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);

    /* 配置输入设备类型 */
    indev_drv.type = LV_INDEV_TYPE_POINTER;

    /* 设置输入设备读取回调函数 */
    indev_drv.read_cb = touchpad_read;

    /* 在LVGL中注册驱动程序，并保存创建的输入设备对象 */
    return lv_indev_drv_register(&indev_drv);
}

/**
 * @brief       lvgl_demo入口函数
 * @param       无
 * @retval      无
 */
void lvgl_demo(void)
{
    /* 初始化LVGL端口配置 */
    lvgl_port_cfg_t lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    /* 初始化LVGL端口 */
    lvgl_port_init(&lvgl_port_cfg);

    lv_port_disp_init();    /* lvgl显示接口初始化,放在lv_init()的后面 */
    lv_port_indev_init();   /* lvgl输入接口初始化,放在lv_init()的后面 */

    /* 锁定互斥锁，因为LVGL API不是线程安全的 */
    if (lvgl_port_lock(0))
    {
        /* 官方demo,需要在SDK Configuration中开启对应Demo */
        // lv_demo_music();      
        // lv_demo_benchmark();
        lv_demo_widgets();
        // lv_demo_stress();
        // lv_demo_keypad_encoder();

        /* 释放互斥锁 */
        lvgl_port_unlock();  /* 释放互斥锁 */
    }
}