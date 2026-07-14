#ifndef _ASM_UAPI_FLUX_IRQ_H
#define _ASM_UAPI_FLUX_IRQ_H

/**
 * flux_set_irq_pending - set an IRQ pending on the current CPU
 */
void flux_set_irq_pending(int irq);

/**
 * flux_set_remote_irq_pending - generate an interrupt on a remote CPU
 *
 * This function is used by the device host side to signal its Linux counterpart
 * that some event happened.
 *
 * @cpu - the cpu number to signal
 * @irq - the irq number to signal
 */
void flux_set_remote_irq_pending(int cpu, int irq);

/**
 * flux_get_free_irq - find and reserve a free IRQ number
 *
 * This function is called by the host device code to find an unused IRQ number
 * and reserved it for its own use.
 *
 * @user - a string to identify the user
 * @returns - and irq number that can be used by request_irq or an negative
 * value in case of an error
 */
int flux_get_free_irq(const char *user);

/**
 * flux_put_irq - release an IRQ number previously obtained with flux_get_free_irq
 *
 * @irq - irq number to release
 */
void flux_put_irq(int irq);

#endif
