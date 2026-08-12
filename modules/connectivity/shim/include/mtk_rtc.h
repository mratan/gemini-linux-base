/* Shim: MTK RTC 32k clock output for GPS — no-op on the 6.6 Base tree.
 *
 * rtc_gpio_enable_32k(RTC_GPIO_USER_GPS) feeds the GPS block's 32k clock via
 * the vendor RTC driver, which the Base tree does not carry. GPS is
 * explicitly out of scope for the Prototype (PRD "Out of Scope"), so this is
 * a safe no-op; Wi-Fi/BT bring-up does not touch this path.
 * See API-CHURN-LOG.md N3 (semantic, novel, GPS-only).
 */
#ifndef __SHIM_MTK_RTC_H__
#define __SHIM_MTK_RTC_H__

typedef enum {
	RTC_GPIO_USER_WIFI = 8,
	RTC_GPIO_USER_GPS = 9,
	RTC_GPIO_USER_BT = 10,
	RTC_GPIO_USER_FM = 11,
	RTC_GPIO_USER_PMIC = 12,
} rtc_gpio_user_t;

static inline void rtc_gpio_enable_32k(rtc_gpio_user_t user) { }
static inline void rtc_gpio_disable_32k(rtc_gpio_user_t user) { }

#endif /* __SHIM_MTK_RTC_H__ */
