#include "stm32f4xx.h"
#include "servo_frames.h"

#define CYCLES_PER_MS 16000u
#define STEP_MS 100u
#define LEASE_MS 350u
#define STEP_DISTANCE 20
#define POSITION_MIN 600
#define POSITION_MAX 2400
#define POSITION_STAMP 0x5a503130u
#define POSITION_RAM ((volatile uint32_t *)0x20008f00u)
#define SERVO_COMMAND_SIZE 15u

extern uint32_t _bss_start, _bss_end;
void entry(void) __attribute__((naked, noreturn));
static void run(void) __attribute__((used, noreturn));
static const uint32_t vectors[] __attribute__((section(".vectors"), used)) = {
    0x20020000u, (uint32_t)entry,
};

static rc_parser_t rx_parser;
static uint8_t peer_present;

static uint32_t peer_tick, step_tick, jog_tick;
static int16_t position, active_code;
static unsigned reply_at, servo_at;
static uint8_t reply_bytes[RC_FRAME_SIZE];
static uint8_t servo_bytes[SERVO_COMMAND_SIZE];

void entry(void)
{
    __asm volatile("cpsid i\n b run");
}

static void mode(GPIO_TypeDef *port, unsigned pin, uint32_t value)
{
    const unsigned shift = 2u * pin;
    port->MODER = (port->MODER & ~(3u << shift)) | (value << shift);
}

static void alternate(GPIO_TypeDef *port, unsigned pin)
{
    const unsigned shift = 4u * (pin % 8u);
    port->AFR[pin / 8u] = (port->AFR[pin / 8u] & ~(15u << shift)) | (7u << shift);
    mode(port, pin, 2u);
}

static void outputs_low(GPIO_TypeDef *port, uint16_t pins)
{
    port->BSRR = (uint32_t)pins << 16;
    for (unsigned pin = 0; pin < 16u; ++pin) {
        if (pins & (1u << pin)) mode(port, pin, 1u);
    }
}

static int stop_pressed(void)
{
    return !(GPIOE->IDR & 1u);
}

static void cancel_motion(void)
{
    active_code = 0;
}

static void schedule_position(uint16_t value)
{
    static const uint8_t command[SERVO_COMMAND_SIZE] = "#255P0000T0100!";
    for (unsigned i = 0; i < SERVO_COMMAND_SIZE; ++i) servo_bytes[i] = command[i];
    for (unsigned place = 9u; place > 5u; --place) {
        servo_bytes[place - 1u] = (uint8_t)('0' + value % 10u);
        value /= 10u;
    }
    servo_at = 0;
}

static void advance_motion(uint32_t now)
{
    if (!active_code) return;
    if ((uint32_t)(now - jog_tick) >= LEASE_MS * CYCLES_PER_MS) {
        cancel_motion();
        return;
    }
    if ((uint32_t)(now - step_tick) < STEP_MS * CYCLES_PER_MS) return;
    int next = position + (active_code == 1620 ? STEP_DISTANCE : -STEP_DISTANCE);
    if (next < POSITION_MIN) next = POSITION_MIN;
    if (next > POSITION_MAX) next = POSITION_MAX;
    if (next == position) return;
    position = (int16_t)next;
    POSITION_RAM[1] = (uint32_t)position;
    step_tick = now;
    schedule_position((uint16_t)position);
}

static void report(uint16_t sequence)
{
    if (reply_at < RC_FRAME_SIZE) return;
    const rc_frame_t frame = {
        SERVO_STATUS, sequence, stop_pressed() ? -1 : active_code, SERVO_BENCH_TAG
    };
    rc_encode(reply_bytes, &frame);
    reply_at = 0;
}

static void accept(const rc_frame_t *frame, uint32_t now)
{
    if (frame->type == RC_STOP) {
        cancel_motion();
        return;
    }
    if (frame->right != SERVO_BENCH_TAG) return;
    if (frame->type == SERVO_PING) {
        peer_present = 1;
        peer_tick = now;
        report(frame->seq);
    } else if (frame->type == SERVO_JOG) {
        if (peer_present && !stop_pressed() && (frame->left == 1380 || frame->left == 1620)) {
            if (!active_code) step_tick = now - STEP_MS * CYCLES_PER_MS;
            active_code = frame->left;
            jog_tick = now;
        }
        report(frame->seq);
    }
}

static void run(void)
{
    for (uint32_t *p = &_bss_start; p < &_bss_end; ++p) *p = 0;
    const uint32_t retained = POSITION_RAM[1];
    position = POSITION_RAM[0] == POSITION_STAMP && retained >= POSITION_MIN && retained <= POSITION_MAX
                   ? (int16_t)retained : 1500;
    POSITION_RAM[1] = (uint32_t)position;
    POSITION_RAM[0] = POSITION_STAMP;
    reply_at = RC_FRAME_SIZE;
    servo_at = SERVO_COMMAND_SIZE;
    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY)) {}
    RCC->CFGR = 0;
    while (RCC->CFGR & RCC_CFGR_SWS) {}
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOCEN |
                    RCC_AHB1ENR_GPIODEN | RCC_AHB1ENR_GPIOEEN | RCC_AHB1ENR_GPIOGEN;
    (void)RCC->AHB1ENR;
    outputs_low(GPIOA, (1u << 4) | (1u << 5));
    outputs_low(GPIOC, (1u << 6) | (1u << 7));
    outputs_low(GPIOD, (1u << 8) | (1u << 9) | (1u << 10) | (1u << 13));
    outputs_low(GPIOE, 0x7f80u | (1u << 15));
    outputs_low(GPIOG, (1u << 6) | (1u << 12));
    GPIOG->BSRR = 1u << 10;
    mode(GPIOG, 10, 1);
    GPIOE->PUPDR = (GPIOE->PUPDR & ~3u) | 1u;
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    (void)RCC->APB2ENR;
    RCC->APB2RSTR |= RCC_APB2RSTR_USART1RST;
    RCC->APB2RSTR &= ~RCC_APB2RSTR_USART1RST;
    USART1->BRR = 139u;
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE;
    alternate(GPIOB, 6);
    alternate(GPIOA, 2);
    alternate(GPIOA, 3);
    USART2->BRR = 139u;
    USART2->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    for (;;) {
        const uint32_t now = DWT->CYCCNT;
        if (stop_pressed() || (peer_present && (uint32_t)(now - peer_tick) >= 300u * CYCLES_PER_MS)) {
            peer_present = 0;
            cancel_motion();
        }
        const uint32_t flags = USART2->SR;
        if (flags & (USART_SR_ORE | USART_SR_NE | USART_SR_FE | USART_SR_PE)) {
            (void)USART2->DR;
            rx_parser.used = 0;
            peer_present = 0;
            cancel_motion();
        } else if (flags & USART_SR_RXNE) {
            rc_frame_t frame;
            if (rc_feed(&rx_parser, (uint8_t)USART2->DR, &frame)) accept(&frame, now);
        }
        advance_motion(now);
        if (reply_at < RC_FRAME_SIZE && (USART2->SR & USART_SR_TXE)) USART2->DR = reply_bytes[reply_at++];
        if (servo_at < SERVO_COMMAND_SIZE && (USART1->SR & USART_SR_TXE)) USART1->DR = servo_bytes[servo_at++];
    }
}\n