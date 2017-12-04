#include <application.h>
#include <bc_eeprom.h>
#include <bc_spi.h>

#define BATTERY_UPDATE_INTERVAL (60 * 60 * 1000)

#define TEMPERATURE_TAG_PUB_NO_CHANGE_INTEVAL (15 * 60 * 1000)
#define TEMPERATURE_TAG_PUB_VALUE_CHANGE 0.2f
#define TEMPERATURE_TAG_UPDATE_INTERVAL (1 * 1000)

#define SET_TEMPERATURE_PUB_INTERVAL 15 * 60 * 1000;
#define SET_TEMPERATURE_ADD_ON_CLICK 0.5f

#define EEPROM_SET_TEMPERATURE_ADDRESS 0
#define APPLICATION_TASK_ID 0

#define COLOR_BLACK true

static bc_led_t led;
static bc_led_t led_lcd_red;
static bc_led_t led_lcd_blue;

static bc_tag_temperature_t temperature;
static event_param_t temperature_event_param = { .next_pub = 0, .value = NAN };
static event_param_t temperature_set_point;
static float temperature_on_display;

static bc_module_lcd_rotation_t rotation;

static bc_lis2dh12_t lis2dh12;
static bc_lis2dh12_result_g_t result;

void radio_pub_set_temperature(void)
{
    temperature_set_point.next_pub = bc_scheduler_get_spin_tick() + SET_TEMPERATURE_PUB_INTERVAL;

    bc_radio_pub_temperature(BC_RADIO_PUB_CHANNEL_SET_POINT, &temperature_set_point.value);
}

void temperature_tag_event_handler(bc_tag_temperature_t *self, bc_tag_temperature_event_t event, void *event_param)
{
    float value;
    event_param_t *param = (event_param_t *)event_param;

    if (event != BC_TAG_TEMPERATURE_EVENT_UPDATE)
    {
        return;
    }

    if (bc_tag_temperature_get_temperature_celsius(self, &value))
    {
        if ((fabs(value - param->value) >= TEMPERATURE_TAG_PUB_VALUE_CHANGE) || (param->next_pub < bc_scheduler_get_spin_tick()))
        {
            bc_radio_pub_temperature(BC_RADIO_PUB_CHANNEL_R1_I2C0_ADDRESS_ALTERNATE, &value);
            param->value = value;
            param->next_pub = bc_scheduler_get_spin_tick() + TEMPERATURE_TAG_PUB_NO_CHANGE_INTEVAL;
        }
    }
    else
    {
        param->value = NAN;
    }

    if (temperature_set_point.next_pub < bc_scheduler_get_spin_tick())
    {
        radio_pub_set_temperature();
    }

    if (param->value != temperature_on_display)
    {
        bc_scheduler_plan_now(APPLICATION_TASK_ID);
    }
}

void lcd_button_event_handler(bc_button_t *self, bc_button_event_t event, void *event_param)
{
    (void) event_param;

	if (event == BC_BUTTON_EVENT_CLICK)
	{
        if (self->_channel.virtual_channel == BC_MODULE_LCD_BUTTON_LEFT)
        {
            temperature_set_point.value -= SET_TEMPERATURE_ADD_ON_CLICK;

            static uint16_t left_event_count = 0;

            left_event_count++;

            bc_radio_pub_event_count(BC_RADIO_PUB_EVENT_LCD_BUTTON_LEFT, &left_event_count);

            bc_led_pulse(&led_lcd_blue, 30);
        }
        else
        {
            temperature_set_point.value += SET_TEMPERATURE_ADD_ON_CLICK;

            static uint16_t right_event_count = 0;

            right_event_count++;

            bc_radio_pub_event_count(BC_RADIO_PUB_EVENT_LCD_BUTTON_RIGHT, &right_event_count);

            bc_led_pulse(&led_lcd_red, 30);
        }

        radio_pub_set_temperature();

        // Save set temperature to eeprom
        uint32_t neg_set_temperature;
        float *set_temperature = (float *) &neg_set_temperature;

        *set_temperature = temperature_set_point.value;

        neg_set_temperature = ~neg_set_temperature;

        bc_eeprom_write(EEPROM_SET_TEMPERATURE_ADDRESS, &temperature_set_point.value, sizeof(temperature_set_point.value));
        bc_eeprom_write(EEPROM_SET_TEMPERATURE_ADDRESS + sizeof(temperature_set_point.value), &neg_set_temperature, sizeof(neg_set_temperature));

        bc_scheduler_plan_now(APPLICATION_TASK_ID);
	}
}

void lis2dh12_event_handler(bc_lis2dh12_t *self, bc_lis2dh12_event_t event, void *event_param)
{
    (void) event_param;

    if (event == BC_LIS2DH12_EVENT_UPDATE)
    {
        bc_lis2dh12_get_result_g(self, &result);

        if ((result.z_axis > 0) && result.z_axis < 0.90)
        {

            if (fabs(result.x_axis) > fabs(result.y_axis))
            {
                if (result.x_axis > 0)
                {
                    rotation = BC_MODULE_LCD_ROTATION_90;
                }
                else
                {
                    rotation = BC_MODULE_LCD_ROTATION_270;
                }
            }
            else
            {
                if (result.y_axis > 0)
                {
                    rotation = BC_MODULE_LCD_ROTATION_0;
                }
                else
                {
                    rotation = BC_MODULE_LCD_ROTATION_180;
                }
            }

            if (rotation != bc_module_lcd_get_rotation())
            {
                bc_scheduler_plan_now(APPLICATION_TASK_ID);
            }
        }
    }
}

void battery_event_handler(bc_module_battery_event_t event, void *event_param)
{
    (void) event;
    (void) event_param;

    float voltage;

    if (bc_module_battery_get_voltage(&voltage))
    {
        bc_radio_pub_battery(&voltage);
    }

}

void application_init(void)
{
    // Initialize LED on core module
    bc_led_init(&led, BC_GPIO_LED, false, false);
    bc_led_set_mode(&led, BC_LED_MODE_OFF);

    // Load set temperature from eeprom
    uint32_t neg_set_temperature;
    float *set_temperature = (float *) &neg_set_temperature;

    bc_eeprom_read(EEPROM_SET_TEMPERATURE_ADDRESS, &temperature_set_point.value, sizeof(temperature_set_point.value));
    bc_eeprom_read(EEPROM_SET_TEMPERATURE_ADDRESS + sizeof(temperature_set_point.value), &neg_set_temperature, sizeof(neg_set_temperature));

    neg_set_temperature = ~neg_set_temperature;

    if (temperature_set_point.value != *set_temperature)
    {
        temperature_set_point.value = 21.0f;
    }

    // Initialize Radio
    bc_radio_init(BC_RADIO_MODE_NODE_SLEEPING);

    // Initialize battery
    bc_module_battery_init(BC_MODULE_BATTERY_FORMAT_MINI);
    bc_module_battery_set_event_handler(battery_event_handler, NULL);
    bc_module_battery_set_update_interval(BATTERY_UPDATE_INTERVAL);

    // Initialize temperature
    bc_tag_temperature_init(&temperature, BC_I2C_I2C0, BC_TAG_TEMPERATURE_I2C_ADDRESS_ALTERNATE);
    bc_tag_temperature_set_update_interval(&temperature, TEMPERATURE_TAG_UPDATE_INTERVAL);
    bc_tag_temperature_set_event_handler(&temperature, temperature_tag_event_handler, &temperature_event_param);

    // Initialize LCD
    bc_module_lcd_init(&_bc_module_lcd_framebuffer);

    // Initialize LCD button left
    static bc_button_t lcd_left;
    bc_button_init_virtual(&lcd_left, BC_MODULE_LCD_BUTTON_LEFT, bc_module_lcd_get_button_driver(), false);
    bc_button_set_event_handler(&lcd_left, lcd_button_event_handler, NULL);

    // Initialize LCD button right
    static bc_button_t lcd_right;
    bc_button_init_virtual(&lcd_right, BC_MODULE_LCD_BUTTON_RIGHT, bc_module_lcd_get_button_driver(), false);
    bc_button_set_event_handler(&lcd_right, lcd_button_event_handler, NULL);

    // Initialize red and blue LED on LCD module
    bc_led_init_virtual(&led_lcd_red, BC_MODULE_LCD_LED_RED, bc_module_lcd_get_led_driver(), true);
    bc_led_init_virtual(&led_lcd_blue, BC_MODULE_LCD_LED_BLUE, bc_module_lcd_get_led_driver(), true);

    // Initialize Accelerometer
    bc_lis2dh12_init(&lis2dh12, BC_I2C_I2C0, 0x19);
    bc_lis2dh12_set_update_interval(&lis2dh12, 100);
    bc_lis2dh12_set_event_handler(&lis2dh12, lis2dh12_event_handler, NULL);

    bc_radio_pairing_request("kit-lcd-thermostat", VERSION);

    bc_led_pulse(&led, 2000);
}

void application_task(void)
{
    static char str_temperature[10];

    if (!bc_module_lcd_is_ready())
    {
    	return;
    }

    bc_module_core_pll_enable();

    bc_module_lcd_set_rotation(rotation);

    bc_module_lcd_clear();

    bc_module_lcd_set_font(&bc_font_ubuntu_33);
    snprintf(str_temperature, sizeof(str_temperature), "%.1f   ", temperature_event_param.value);
    int x = bc_module_lcd_draw_string(20, 20, str_temperature, COLOR_BLACK);
    temperature_on_display = temperature_event_param.value;

    bc_module_lcd_set_font(&bc_font_ubuntu_24);
    bc_module_lcd_draw_string(x - 20, 25, "\xb0" "C   ", COLOR_BLACK);

    bc_module_lcd_set_font(&bc_font_ubuntu_15);
    bc_module_lcd_draw_string(10, 80, "Set temperature", COLOR_BLACK);

    snprintf(str_temperature, sizeof(str_temperature), "%.1f \xb0" "C", temperature_set_point.value);
    bc_module_lcd_draw_string(40, 100, str_temperature, COLOR_BLACK);

    bc_module_lcd_update();

    bc_module_core_pll_disable();
}