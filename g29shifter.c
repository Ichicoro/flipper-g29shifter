#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_gpio.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/widget.h>
#include <gui/modules/variable_item_list.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include "g29shifter_icons.h"

#include <stm32wbxx_ll_adc.h>
#include <stm32wbxx_ll_pwr.h>

#define TAG "Skeleton"

#define X_AXIS_GPIO LL_GPIO_PIN_3;
#define Y_AXIS_GPIO LL_GPIO_PIN_4;

// Change this to BACKLIGHT_AUTO if you don't want the backlight to be continuously on.
#define BACKLIGHT_ON 1

// Our application menu has 3 items.  You can add more items if you want.
typedef enum {
    SkeletonSubmenuIndexConfigure,
    SkeletonSubmenuIndexGame,
    SkeletonSubmenuIndexAbout,
} SkeletonSubmenuIndex;

// Each view is a screen we show the user.
typedef enum {
    SkeletonViewTextInput, // Input for configuring text settings
    SkeletonViewConfigure, // The configuration screen
    SkeletonViewGame, // The main screen
    SkeletonViewAbout, // The about screen with directions, link to social channel, etc.
} SkeletonView;

typedef enum {
    SkeletonEventIdRedrawScreen = 0, // Custom event to redraw the screen
    SkeletonEventIdOkPressed = 42, // Custom event to process OK button getting pressed down
} SkeletonEventId;

typedef struct {
    float x;
    float y;
    bool reverse;
} AppInputModel;

typedef struct {
    ViewDispatcher* view_dispatcher; // Switches between our views
    NotificationApp* notifications; // Used for controlling the backlight
    Submenu* submenu; // The application menu
    TextInput* text_input; // The text input screen
    VariableItemList* variable_item_list_config; // The configuration screen
    View* view_game; // The main screen
    Widget* widget_about; // The about screen

    VariableItem* setting_2_item; // The name setting item (so we can update the text)
    char* temp_buffer; // Temporary buffer for text input
    uint32_t temp_buffer_size; // Size of temporary buffer

    FuriTimer* timer; // Timer for redrawing the screen
} SkeletonApp;

typedef enum {
    FuriHalVref2048,
    FuriHalVref2500,
} FuriHalVref;

void furi_hal_adc_set_vref(FuriHalVref vref) {
    furi_assert(vref == FuriHalVref2048 || vref == FuriHalVref2500);
    uint32_t trim_value = 0;

    switch(vref) {
    case FuriHalVref2048:
        LL_VREFBUF_SetVoltageScaling(LL_VREFBUF_VOLTAGE_SCALE0);
        trim_value = LL_VREFBUF_SC0_GetCalibration() & 0x3FU;
        break;
    case FuriHalVref2500:
        LL_VREFBUF_SetVoltageScaling(LL_VREFBUF_VOLTAGE_SCALE1);
        trim_value = LL_VREFBUF_SC1_GetCalibration() & 0x3FU;
        break;
    }

    LL_VREFBUF_SetTrimming(trim_value);
}

void furi_hal_adc_init() {
    LL_VREFBUF_Enable();
    LL_VREFBUF_DisableHIZ();

    // FHCLK ≥ FADC / 4 if the resolution of all channels are 12-bit or 10-bit
    // STM32WB55xx ADC maximum frequency is 64MHz (corresponding to 4.27Msmp/s maximum)
    // TODO: calculate correct clock
    LL_ADC_SetCommonClock(__LL_ADC_COMMON_INSTANCE(ADC1), LL_ADC_CLOCK_SYNC_PCLK_DIV4);

    LL_ADC_SetResolution(ADC1, LL_ADC_RESOLUTION_12B);
    LL_ADC_SetDataAlignment(ADC1, LL_ADC_DATA_ALIGN_RIGHT);
    LL_ADC_SetLowPowerMode(ADC1, LL_ADC_LP_MODE_NONE);
    ADC1->CFGR |= 0x00008000; //ADC Auto-off enable

    LL_ADC_REG_SetTriggerSource(ADC1, LL_ADC_REG_TRIG_SOFTWARE);
    LL_ADC_REG_SetSequencerLength(ADC1, LL_ADC_REG_SEQ_SCAN_DISABLE);
    LL_ADC_REG_SetSequencerDiscont(ADC1, LL_ADC_REG_SEQ_DISCONT_DISABLE);
    LL_ADC_REG_SetContinuousMode(ADC1, LL_ADC_REG_CONV_SINGLE);
    LL_ADC_REG_SetDMATransfer(ADC1, LL_ADC_REG_DMA_TRANSFER_NONE);
    LL_ADC_REG_SetOverrun(ADC1, LL_ADC_REG_OVR_DATA_OVERWRITTEN);

    LL_ADC_SetOverSamplingScope(ADC1, LL_ADC_OVS_DISABLE);

    LL_ADC_DisableDeepPowerDown(ADC1);

    LL_ADC_EnableInternalRegulator(ADC1);
    furi_delay_us(LL_ADC_DELAY_INTERNAL_REGUL_STAB_US);

    LL_ADC_StartCalibration(ADC1, LL_ADC_SINGLE_ENDED);
    while(LL_ADC_IsCalibrationOnGoing(ADC1) != 0) {
        // TODO: add timeout
    }

    // TODO: calculate delay between ADC end of calibration and ADC enable
    furi_delay_us(100);
}

void furi_hal_adc_enable() {
    LL_ADC_Enable(ADC1);
    while(LL_ADC_IsEnabled(ADC1) == 0) {
        // while(LL_ADC_IsActiveFlag_ADRDY(ADC1) == 0) {
        // TODO: add timeout
    }

    // TODO: find out if the SMPS can be bypassed
    // /* Disable SMPS: SMPS in mode step-down can impact ADC conversion accuracy. */
    // /* It is recommnended to disable SMPS (stop SMPS switching by setting it    */
    // /* in mode bypass) during ADC conversion.                                   */
    // /* Get SMPS effective operating mode */
    // if(LL_PWR_SMPS_GetEffectiveMode() == LL_PWR_SMPS_STEP_DOWN) {
    //     /* Set SMPS operating mode */
    //     LL_PWR_SMPS_SetMode(LL_PWR_SMPS_BYPASS);
    // }
}

/**
 * @brief disable ADC
 * Prerequisites: ADC conversions must be stopped.
 * 
 */
void furi_hal_adc_disable() {
    LL_ADC_Disable(ADC1);
    while(LL_ADC_IsEnabled(ADC1)) {
        // TODO: add timeout
    }
}

void furi_hal_adc_deinit() {
    LL_ADC_DisableInternalRegulator(ADC1);
    LL_ADC_EnableDeepPowerDown(ADC1);
    LL_VREFBUF_EnableHIZ();
    LL_VREFBUF_Disable();
}

typedef enum {
    FuriHalAdcChannel0 = LL_ADC_CHANNEL_0,
    FuriHalAdcChannel1 = LL_ADC_CHANNEL_1,
    FuriHalAdcChannel2 = LL_ADC_CHANNEL_2,
    FuriHalAdcChannel3 = LL_ADC_CHANNEL_3,
    FuriHalAdcChannel4 = LL_ADC_CHANNEL_4,
    FuriHalAdcChannel5 = LL_ADC_CHANNEL_5,
    FuriHalAdcChannel6 = LL_ADC_CHANNEL_6,
    FuriHalAdcChannel7 = LL_ADC_CHANNEL_7,
    FuriHalAdcChannel8 = LL_ADC_CHANNEL_8,
    FuriHalAdcChannel9 = LL_ADC_CHANNEL_9,
    FuriHalAdcChannel10 = LL_ADC_CHANNEL_10,
    FuriHalAdcChannel11 = LL_ADC_CHANNEL_11,
    FuriHalAdcChannel12 = LL_ADC_CHANNEL_12,
    FuriHalAdcChannel13 = LL_ADC_CHANNEL_13,
    FuriHalAdcChannel14 = LL_ADC_CHANNEL_14,
    FuriHalAdcChannel15 = LL_ADC_CHANNEL_15,
    FuriHalAdcChannel16 = LL_ADC_CHANNEL_16,
    FuriHalAdcChannel17 = LL_ADC_CHANNEL_17,
    FuriHalAdcChannel18 = LL_ADC_CHANNEL_18,
    FuriHalAdcChannelVREFINT = LL_ADC_CHANNEL_VREFINT,
    FuriHalAdcChannelTEMPSENSOR = LL_ADC_CHANNEL_TEMPSENSOR,
    FuriHalAdcChannelVBAT = LL_ADC_CHANNEL_VBAT,
} FuriHalAdcChannel;

/**
 * @brief Set single channel for ADC
 * ADC has a 18 channels, and channels 0-5 are "fast" channels.
 * Fast channels have ADC conversion rate up to 5.33 Ms/s (0.188 us for 12-bit resolution).
 * Slow channels have ADC conversion rate up to 4.21 Ms/s (0.238 us for 12-bit resolution).
 * @param channel 
 */
void furi_hal_adc_set_single_channel(FuriHalAdcChannel channel) {
    LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_1, channel);
    // TODO: calculate sampling time
    LL_ADC_SetChannelSamplingTime(ADC1, channel, LL_ADC_SAMPLINGTIME_2CYCLES_5);
    LL_ADC_SetChannelSingleDiff(ADC1, channel, LL_ADC_SINGLE_ENDED);
}

/**
 * @brief Read ADC value by software trigger
 * 
 * @return uint32_t ADC value
 */
uint32_t furi_hal_adc_read_sw() {
    LL_ADC_REG_StartConversion(ADC1);
    while(LL_ADC_IsActiveFlag_EOC(ADC1) == 0) {
        // FURI_LOG_I(TAG, "ADC EOC: %ld", LL_ADC_IsActiveFlag_EOC(ADC1));
    }
    return LL_ADC_REG_ReadConversionData12(ADC1);
}

/**
 * @brief      Callback for exiting the application.
 * @details    This function is called when user press back button.  We return VIEW_NONE to
 *            indicate that we want to exit the application.
 * @param      _context  The context - unused
 * @return     next view id
*/
static uint32_t skeleton_navigation_exit_callback(void* _context) {
    UNUSED(_context);

    furi_hal_adc_disable();
    furi_hal_adc_deinit();
    return VIEW_NONE;
}

/**
 * @brief      Callback for returning to submenu.
 * @details    This function is called when user press back button.  We return VIEW_NONE to
 *            indicate that we want to navigate to the submenu.
 * @param      _context  The context - unused
 * @return     next view id
*/
static uint32_t skeleton_navigation_submenu_callback(void* _context) {
    UNUSED(_context);
    return SkeletonViewGame;
}

/**
 * @brief      Callback for drawing the game screen.
 * @details    This function is called when the screen needs to be redrawn, like when the model gets updated.
 * @param      canvas  The canvas to draw on.
 * @param      model   The model - MyModel object.
*/
static void view_draw_callback(Canvas* canvas, void* model) {
    AppInputModel* my_model = (AppInputModel*)model;
    UNUSED(my_model);

    FuriString* xy_str = furi_string_alloc();
    if(my_model->reverse) {
        furi_string_printf(
            xy_str, "X: %.2lf / Y: %.2lf [R]", (double)my_model->x, (double)my_model->y);
    } else {
        furi_string_printf(
            xy_str, "X: %.2lf / Y: %.2lf", (double)my_model->x, (double)my_model->y);
    }

    canvas_set_bitmap_mode(canvas, true);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 13, 18, "Logitech G29 Shifter");
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str(canvas, 103, 53, "0");
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 13, 50, "Selected gear:");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 13, 60, furi_string_get_cstr(xy_str));

    furi_string_free(xy_str);
}

/**
 * @brief      Callback for timer elapsed.
 * @details    This function is called when the timer is elapsed.  We use this to queue a redraw event.
 * @param      context  The context - SkeletonApp object.
*/
static void skeleton_view_game_timer_callback(void* context) {
    SkeletonApp* app = (SkeletonApp*)context;
    AppInputModel* model = view_get_model(app->view_game);
    view_dispatcher_send_custom_event(app->view_dispatcher, SkeletonEventIdRedrawScreen);

    furi_hal_adc_set_single_channel(FuriHalAdcChannel9);
    uint32_t adc_value_x = furi_hal_adc_read_sw();
    LL_ADC_REG_StopConversion(ADC1);
    float adc_voltage_x = 2.5f * (float)adc_value_x / 4096.0f;
    model->x = adc_voltage_x;

    // FURI_LOG_I(TAG, "ADCX: %ld, %f V", adc_value_x, (double)adc_voltage_x);

    furi_hal_adc_set_single_channel(FuriHalAdcChannel11);
    uint32_t adc_value_y = furi_hal_adc_read_sw();
    LL_ADC_REG_StopConversion(ADC1);
    float adc_voltage_y = 2.5f * (float)adc_value_y / 4096.0f;
    model->y = adc_voltage_y;

    model->reverse = furi_hal_gpio_read(&gpio_ext_pc1);

    FURI_LOG_I(
        TAG,
        "ADCX: %ld, %f V; ADCY: %ld, %f V",
        adc_value_x,
        (double)adc_voltage_x,
        adc_value_y,
        (double)adc_voltage_y);
}

/**
 * @brief      Callback when the user starts the game screen.
 * @details    This function is called when the user enters the game screen.  We start a timer to
 *           redraw the screen periodically (so the random number is refreshed).
 * @param      context  The context - SkeletonApp object.
*/
static void skeleton_view_game_enter_callback(void* context) {
    uint32_t period = furi_ms_to_ticks(200);
    SkeletonApp* app = (SkeletonApp*)context;
    furi_assert(app->timer == NULL);
    app->timer =
        furi_timer_alloc(skeleton_view_game_timer_callback, FuriTimerTypePeriodic, context);
    furi_timer_start(app->timer, period);
}

/**
 * @brief      Callback when the user exits the game screen.
 * @details    This function is called when the user exits the game screen.  We stop the timer.
 * @param      context  The context - SkeletonApp object.
*/
static void skeleton_view_game_exit_callback(void* context) {
    SkeletonApp* app = (SkeletonApp*)context;
    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);
    app->timer = NULL;
}

/**
 * @brief      Callback for custom events.
 * @details    This function is called when a custom event is sent to the view dispatcher.
 * @param      event    The event id - SkeletonEventId value.
 * @param      context  The context - SkeletonApp object.
*/
static bool skeleton_view_game_custom_event_callback(uint32_t event, void* context) {
    SkeletonApp* app = (SkeletonApp*)context;
    UNUSED(app);
    switch(event) {
    case SkeletonEventIdRedrawScreen:
        // Redraw screen by passing true to last parameter of with_view_model.
        {
            bool redraw = true;
            with_view_model(
                app->view_game, AppInputModel * _model, { UNUSED(_model); }, redraw);
            return true;
        }
    case SkeletonEventIdOkPressed:
        // Process the OK button.  We play a tone based on the x coordinate.
        if(furi_hal_speaker_acquire(500)) {
            float frequency = 100;
            // bool redraw = false;
            // with_view_model(
            //     app->view_game,
            //     SkeletonGameModel * model,
            //     { frequency = model->x * 100 + 100; },
            //     redraw);
            furi_hal_speaker_start(frequency, 1.0);
            furi_delay_ms(100);
            furi_hal_speaker_stop();
            furi_hal_speaker_release();
        }
        return true;
    default:
        return false;
    }
}

/**
 * @brief      Callback for game screen input.
 * @details    This function is called when the user presses a button while on the game screen.
 * @param      event    The event - InputEvent object.
 * @param      context  The context - SkeletonApp object.
 * @return     true if the event was handled, false otherwise.
*/
static bool skeleton_view_game_input_callback(InputEvent* event, void* context) {
    SkeletonApp* app = (SkeletonApp*)context;
    if(event->type == InputTypeShort) {
        if(event->key == InputKeyLeft) {
            // prova
        } else if(event->key == InputKeyRight) {
            // prova
        }
    } else if(event->type == InputTypePress) {
        if(event->key == InputKeyOk) {
            // We choose to send a custom event when user presses OK button.  skeleton_custom_event_callback will
            // handle our SkeletonEventIdOkPressed event.  We could have just put the code from
            // skeleton_custom_event_callback here, it's a matter of preference.
            view_dispatcher_send_custom_event(app->view_dispatcher, SkeletonEventIdOkPressed);
            return true;
        }
    }

    return false;
}

static void skeleton_app_init_gpio() {
    furi_hal_bus_enable(FuriHalBusADC);
    furi_hal_gpio_init(&gpio_ext_pa4, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    furi_hal_gpio_init(&gpio_ext_pa6, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    furi_hal_gpio_init(&gpio_ext_pc1, GpioModeInput, GpioPullNo, GpioSpeedLow);

    furi_hal_adc_init();
    FURI_LOG_I(TAG, "ADC Init OK");

    furi_hal_adc_set_vref(FuriHalVref2500);
    FURI_LOG_I(TAG, "Vref Set OK");

    furi_hal_adc_set_single_channel(FuriHalAdcChannel4);
    FURI_LOG_I(TAG, "ADC Set Channel OK");

    furi_hal_adc_enable();
    FURI_LOG_I(TAG, "ADC Enable OK");
}

/**
 * @brief      Allocate the skeleton application.
 * @details    This function allocates the skeleton application resources.
 * @return     SkeletonApp object.
*/
static SkeletonApp* skeleton_app_alloc() {
    SkeletonApp* app = (SkeletonApp*)malloc(sizeof(SkeletonApp));

    skeleton_app_init_gpio();

    Gui* gui = furi_record_open(RECORD_GUI);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_enable_queue(app->view_dispatcher);
    view_dispatcher_attach_to_gui(app->view_dispatcher, gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);

    app->view_game = view_alloc();
    view_set_draw_callback(app->view_game, view_draw_callback);
    view_set_input_callback(app->view_game, skeleton_view_game_input_callback);
    view_set_previous_callback(app->view_game, skeleton_navigation_exit_callback);
    view_set_enter_callback(app->view_game, skeleton_view_game_enter_callback);
    view_set_exit_callback(app->view_game, skeleton_view_game_exit_callback);
    view_set_context(app->view_game, app);
    view_set_custom_callback(app->view_game, skeleton_view_game_custom_event_callback);
    view_allocate_model(app->view_game, ViewModelTypeLockFree, sizeof(AppInputModel));
    AppInputModel* model = view_get_model(app->view_game);
    model->x = 0;
    model->y = 0;
    model->reverse = false;
    view_dispatcher_add_view(app->view_dispatcher, SkeletonViewGame, app->view_game);
    view_dispatcher_switch_to_view(app->view_dispatcher, SkeletonViewGame);

    app->text_input = text_input_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, SkeletonViewTextInput, text_input_get_view(app->text_input));
    app->temp_buffer_size = 32;
    app->temp_buffer = (char*)malloc(app->temp_buffer_size);

    app->widget_about = widget_alloc();
    widget_add_text_scroll_element(
        app->widget_about,
        0,
        0,
        128,
        64,
        "This is a sample application.\n---\nReplace code and message\nwith your content!\n\nauthor: @codeallnight\nhttps://discord.com/invite/NsjCvqwPAd\nhttps://youtube.com/@MrDerekJamison");
    view_set_previous_callback(
        widget_get_view(app->widget_about), skeleton_navigation_submenu_callback);
    view_dispatcher_add_view(
        app->view_dispatcher, SkeletonViewAbout, widget_get_view(app->widget_about));

    app->notifications = furi_record_open(RECORD_NOTIFICATION);

#ifdef BACKLIGHT_ON
    notification_message(app->notifications, &sequence_display_backlight_enforce_on);
#endif

    return app;
}

/**
 * @brief      Free the skeleton application.
 * @details    This function frees the skeleton application resources.
 * @param      app  The skeleton application object.
*/
static void skeleton_app_free(SkeletonApp* app) {
#ifdef BACKLIGHT_ON
    notification_message(app->notifications, &sequence_display_backlight_enforce_auto);
#endif
    furi_record_close(RECORD_NOTIFICATION);

    view_dispatcher_remove_view(app->view_dispatcher, SkeletonViewTextInput);
    text_input_free(app->text_input);
    free(app->temp_buffer);
    view_dispatcher_remove_view(app->view_dispatcher, SkeletonViewAbout);
    widget_free(app->widget_about);
    view_dispatcher_remove_view(app->view_dispatcher, SkeletonViewGame);
    view_free(app->view_game);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);

    free(app);
}

/**
 * @brief      Main function for skeleton application.
 * @details    This function is the entry point for the skeleton application.  It should be defined in
 *           application.fam as the entry_point setting.
 * @param      _p  Input parameter - unused
 * @return     0 - Success
*/
int32_t g29shifter_app(void* _p) {
    UNUSED(_p);

    SkeletonApp* app = skeleton_app_alloc();
    view_dispatcher_run(app->view_dispatcher);

    skeleton_app_free(app);
    return 0;
}
