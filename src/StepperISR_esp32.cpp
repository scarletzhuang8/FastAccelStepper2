
#include "StepperISR.h"

#if defined(ARDUINO_ARCH_ESP32)

#define DEFAULT_TIMER_H_L_TRANSITION 160

// cannot be updated while timer is running => fix it to 0
#define TIMER_PRESCALER 0

//#define TEST_PROBE 18

// Here are the global variables to interface with the interrupts
StepperQueue fas_queue[NUM_QUEUES];

// Here the associated mapping from queue to mcpwm/pcnt units
static const struct mapping_s queue2mapping[NUM_QUEUES] = {
    {
      mcpwm_unit : MCPWM_UNIT_0,
      timer : 0,
      pwm_output_pin : MCPWM0A,
      pcnt_unit : PCNT_UNIT_0,
      input_sig_index : PCNT_SIG_CH0_IN0_IDX,
      cmpr_tea_int_clr : MCPWM_OP0_TEA_INT_CLR,
      cmpr_tea_int_ena : MCPWM_OP0_TEA_INT_ENA,
      cmpr_tea_int_raw : MCPWM_OP0_TEA_INT_RAW
    },
    {
      mcpwm_unit : MCPWM_UNIT_0,
      timer : 1,
      pwm_output_pin : MCPWM1A,
      pcnt_unit : PCNT_UNIT_1,
      input_sig_index : PCNT_SIG_CH0_IN1_IDX,
      cmpr_tea_int_clr : MCPWM_OP1_TEA_INT_CLR,
      cmpr_tea_int_ena : MCPWM_OP1_TEA_INT_ENA,
      cmpr_tea_int_raw : MCPWM_OP1_TEA_INT_RAW
    },
    {
      mcpwm_unit : MCPWM_UNIT_0,
      timer : 2,
      pwm_output_pin : MCPWM2A,
      pcnt_unit : PCNT_UNIT_2,
      input_sig_index : PCNT_SIG_CH0_IN2_IDX,
      cmpr_tea_int_clr : MCPWM_OP2_TEA_INT_CLR,
      cmpr_tea_int_ena : MCPWM_OP2_TEA_INT_ENA,
      cmpr_tea_int_raw : MCPWM_OP2_TEA_INT_RAW
    },
    {
      mcpwm_unit : MCPWM_UNIT_1,
      timer : 0,
      pwm_output_pin : MCPWM0A,
      pcnt_unit : PCNT_UNIT_3,
      input_sig_index : PCNT_SIG_CH0_IN3_IDX,
      cmpr_tea_int_clr : MCPWM_OP0_TEA_INT_CLR,
      cmpr_tea_int_ena : MCPWM_OP0_TEA_INT_ENA,
      cmpr_tea_int_raw : MCPWM_OP0_TEA_INT_RAW
    },
    {
      mcpwm_unit : MCPWM_UNIT_1,
      timer : 1,
      pwm_output_pin : MCPWM1A,
      pcnt_unit : PCNT_UNIT_4,
      input_sig_index : PCNT_SIG_CH0_IN4_IDX,
      cmpr_tea_int_clr : MCPWM_OP1_TEA_INT_CLR,
      cmpr_tea_int_ena : MCPWM_OP1_TEA_INT_ENA,
      cmpr_tea_int_raw : MCPWM_OP1_TEA_INT_RAW
    },
    {
      mcpwm_unit : MCPWM_UNIT_1,
      timer : 2,
      pwm_output_pin : MCPWM2A,
      pcnt_unit : PCNT_UNIT_5,
      input_sig_index : PCNT_SIG_CH0_IN5_IDX,
      cmpr_tea_int_clr : MCPWM_OP2_TEA_INT_CLR,
      cmpr_tea_int_ena : MCPWM_OP2_TEA_INT_ENA,
      cmpr_tea_int_raw : MCPWM_OP2_TEA_INT_RAW
    },
};

void IRAM_ATTR apply_command(StepperQueue *queue, const struct queue_entry *e) {
  const struct mapping_s *mapping = queue->mapping;
  mcpwm_unit_t mcpwm_unit = mapping->mcpwm_unit;
  mcpwm_dev_t *mcpwm = mcpwm_unit == MCPWM_UNIT_0 ? &MCPWM0 : &MCPWM1;
  pcnt_unit_t pcnt_unit = mapping->pcnt_unit;
  uint8_t timer = mapping->timer;
  uint8_t steps = e->steps;
  if (e->toggle_dir) {
    uint8_t dirPin = queue->dirPin;
    digitalWrite(dirPin, digitalRead(dirPin) == HIGH ? LOW : HIGH);
  }
  uint16_t ticks = e->ticks;
  if (mcpwm->timer[timer].status.value <= 1) {
    mcpwm->timer[timer].period.upmethod = 0;  // 0 = immediate update, 1 = TEZ
  } else {
    mcpwm->timer[timer].period.upmethod = 1;  // 0 = immediate update, 1 = TEZ
  }
  mcpwm->timer[timer].period.period = ticks;
  if (steps == 0) {
    // timer value = 1 - upcounting: output low
    mcpwm->channel[timer].generator[0].utea = 1;
    mcpwm->int_clr.val = mapping->cmpr_tea_int_clr;
    mcpwm->int_ena.val |= mapping->cmpr_tea_int_ena;
  } else {
    // For fast pulses, eventually the ISR is late. So take the current pulse
    // count into consideration.
    //
    // Automatic update for next pulse cnt on pulse counter zero does not work.
    // For example the sequence:
    //		5 pulses
    //		1 pause		==> here need to store 3, but not available yet
    //		3 pulses
    //
    // Read counter
    uint16_t val1 = steps - PCNT.cnt_unit[pcnt_unit].cnt_val;
    // Clear flag for l-->h transition
    mcpwm->int_clr.val = mapping->cmpr_tea_int_clr;
    // Read counter again
    uint16_t val2 = steps - PCNT.cnt_unit[pcnt_unit].cnt_val;
    // If no pulse arrives between val1 and val2:
    //		val2 == val1
    // If pulse arrives between val1 and int_clr:
    //		mcpwm status is cleared and val2 == val1+1
    // If pulse arrives between int_clr and val2:
    //		mcpwm status is set and val2 == val1+1
    // => mcwpm status info is not reliable, so clear again
    if (val1 != val2) {
      // Clear flag again. No pulse can be expected between val2 and here
      mcpwm->int_clr.val = mapping->cmpr_tea_int_clr;
    }

    // is updated only on zero
    PCNT.conf_unit[pcnt_unit].conf2.cnt_h_lim = val2;
    // force take over
    pcnt_counter_clear(pcnt_unit);
    // Check, if pulse has come in
    if ((mcpwm->int_raw.val & mapping->cmpr_tea_int_raw) != 0) {
      // Need to adjust one down
      // Here the border case = 1 is ignored, because the command rate is
      // limited
      //
      // Check if the pulse has been counted or not
      if (PCNT.cnt_unit[pcnt_unit].cnt_val == val2) {
        // pulse hasn't been counted, so adjust the limit
        // is updated only on zero
        PCNT.conf_unit[pcnt_unit].conf2.cnt_h_lim = val2 - 1;
        // force take over
        pcnt_counter_clear(pcnt_unit);
      }
    }
    // disable mcpwm interrupt
    mcpwm->int_ena.val &= ~mapping->cmpr_tea_int_ena;
    // timer value = 1 - upcounting: output high
    mcpwm->channel[timer].generator[0].utea = 2;
  }
}

static void IRAM_ATTR init_stop(StepperQueue *q) {
  // init stop is normally called after the first command,
  // because the second command is entered too late
  // and after the last command aka running out of commands.
  const struct mapping_s *mapping = q->mapping;
  mcpwm_unit_t mcpwm_unit = mapping->mcpwm_unit;
  mcpwm_dev_t *mcpwm = mcpwm_unit == MCPWM_UNIT_0 ? &MCPWM0 : &MCPWM1;
  uint8_t timer = mapping->timer;
  mcpwm->timer[timer].mode.start = 0;  // 0: stop at TEZ
  // timer value = 1 - upcounting: output low
  mcpwm->int_ena.val &= ~mapping->cmpr_tea_int_ena;
  // PCNT.conf_unit[mapping->pcnt_unit].conf2.cnt_h_lim = 1;
  q->queue_end.ticks = TICKS_FOR_STOPPED_MOTOR;
  q->_hasISRactive = false;
}

static void IRAM_ATTR what_is_next(StepperQueue *q) {
  uint8_t rp = q->read_idx;
  if (rp != q->next_write_idx) {
    struct queue_entry *e = &q->entry[rp & QUEUE_LEN_MASK];
    apply_command(q, e);
    q->read_idx = rp + 1;
  } else {
    // no more commands: stop timer at period end
    init_stop(q);
  }
}

static void IRAM_ATTR pcnt_isr_service(void *arg) {
  StepperQueue *q = (StepperQueue *)arg;
  what_is_next(q);
}

// MCPWM_SERVICE is only used in case of pause
#define MCPWM_SERVICE(mcpwm, TIMER, pcnt)             \
  if (mcpwm.int_st.cmpr##TIMER##_tea_int_st != 0) {   \
    /*managed in apply_command()                   */ \
    /*mcpwm.int_clr.cmpr##TIMER##_tea_int_clr = 1;*/  \
    StepperQueue *q = &fas_queue[pcnt];               \
    what_is_next(q);                                  \
  }

static void IRAM_ATTR mcpwm0_isr_service(void *arg) {
  // For whatever reason, this interrupt is constantly called even with int_st =
  // 0 while the timer is running
  MCPWM_SERVICE(MCPWM0, 0, 0);
  MCPWM_SERVICE(MCPWM0, 1, 1);
  MCPWM_SERVICE(MCPWM0, 2, 2);
}
static void IRAM_ATTR mcpwm1_isr_service(void *arg) {
  MCPWM_SERVICE(MCPWM1, 0, 3);
  MCPWM_SERVICE(MCPWM1, 1, 4);
  MCPWM_SERVICE(MCPWM1, 2, 5);
}

void StepperQueue::init(uint8_t queue_num, uint8_t step_pin) {
#ifdef TEST_PROBE
  pinMode(TEST_PROBE, OUTPUT);
#endif

  _initVars();
  _step_pin = step_pin;

  mapping = &queue2mapping[queue_num];

  mcpwm_unit_t mcpwm_unit = mapping->mcpwm_unit;
  mcpwm_dev_t *mcpwm = mcpwm_unit == MCPWM_UNIT_0 ? &MCPWM0 : &MCPWM1;
  pcnt_unit_t pcnt_unit = mapping->pcnt_unit;
  uint8_t timer = mapping->timer;

  pcnt_config_t cfg;
  // if step_pin is not set here (or 0x30), then it does not work
  cfg.pulse_gpio_num = step_pin;          // static 0 is 0x30, static 1 is 0x38
  cfg.ctrl_gpio_num = PCNT_PIN_NOT_USED;  // static 0 is 0x30, static 1 is 0x38
  cfg.lctrl_mode = PCNT_MODE_KEEP;
  cfg.hctrl_mode = PCNT_MODE_KEEP;
  cfg.pos_mode = PCNT_COUNT_INC;  // increment on rising edge
  cfg.neg_mode = PCNT_COUNT_DIS;  // ignore falling edge
  cfg.counter_h_lim = 1;
  cfg.counter_l_lim = 0;
  cfg.unit = pcnt_unit;
  cfg.channel = PCNT_CHANNEL_0;
  pcnt_unit_config(&cfg);

  PCNT.conf_unit[pcnt_unit].conf2.cnt_h_lim = 1;
  PCNT.conf_unit[pcnt_unit].conf0.thr_h_lim_en = 1;
  PCNT.conf_unit[pcnt_unit].conf0.thr_l_lim_en = 0;

  pcnt_counter_clear(pcnt_unit);
  pcnt_counter_resume(pcnt_unit);
  pcnt_event_enable(pcnt_unit, PCNT_EVT_H_LIM);
  if (queue_num == 0) {
    // isr_service_install apparently enables the interrupt
    PCNT.int_clr.val = PCNT.int_st.val;
    pcnt_isr_service_install(ESP_INTR_FLAG_SHARED | ESP_INTR_FLAG_IRAM);
  }
  pcnt_isr_handler_add(pcnt_unit, pcnt_isr_service, (void *)this);

  if (timer == 0) {
    // Init mcwpm module for use
    periph_module_enable(mcpwm_unit == MCPWM_UNIT_0 ? PERIPH_PWM0_MODULE
                                                    : PERIPH_PWM1_MODULE);
    mcpwm->int_ena.val = 0;  // disable all interrupts
    mcpwm_isr_register(
        mcpwm_unit,
        mcpwm_unit == MCPWM_UNIT_0 ? mcpwm0_isr_service : mcpwm1_isr_service,
        NULL, ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_SHARED, NULL);

    // 160 MHz/5 = 32 MHz => 16 MHz in up/down-mode
    mcpwm->clk_cfg.prescale = 5 - 1;

    mcpwm->timer_sel.operator0_sel = 0;  // timer 0 is input for operator 0
    mcpwm->timer_sel.operator1_sel = 1;  // timer 1 is input for operator 1
    mcpwm->timer_sel.operator2_sel = 2;  // timer 2 is input for operator 2
  }
  mcpwm->timer[timer].period.upmethod = 1;  // 0 = immediate update, 1 = TEZ
  mcpwm->timer[timer].period.prescale = TIMER_PRESCALER;
  mcpwm->timer[timer].period.period = 400;  // Random value
  mcpwm->timer[timer].mode.mode = 3;        // 3=up/down counting
  mcpwm->timer[timer].mode.start = 0;       // 0: stop at TEZ

  // this sequence should reset the timer to 0
  mcpwm->timer[timer].sync.timer_phase = 0;  // prepare value of 0
  mcpwm->timer[timer].sync.in_en = 1;        // enable sync
  mcpwm->timer[timer].sync.sync_sw ^= 1;     // force a sync
  mcpwm->timer[timer].sync.in_en = 0;        // disable sync

  mcpwm->channel[timer].cmpr_cfg.a_upmethod = 0;     // 0 = immediate update
  mcpwm->channel[timer].cmpr_value[0].cmpr_val = 1;  // set compare value A
  mcpwm->channel[timer].generator[0].val = 0;   // clear all trigger actions
  mcpwm->channel[timer].generator[1].val = 0;   // clear all trigger actions
  mcpwm->channel[timer].generator[0].dtep = 1;  // low at period
  mcpwm->channel[timer].db_cfg.val = 0;         // edge delay disabled
  mcpwm->channel[timer].carrier_cfg.val = 0;    // carrier disabled

  digitalWrite(step_pin, LOW);
  pinMode(step_pin, OUTPUT);

  // at last, link mcpwm to output pin and back into pcnt input
  connect();
}

void StepperQueue::connect() {
  mcpwm_unit_t mcpwm_unit = mapping->mcpwm_unit;
  mcpwm_gpio_init(mcpwm_unit, mapping->pwm_output_pin, _step_pin);
  // Doesn't work with gpio_matrix_in
  //  gpio_matrix_in(step_pin, mapping->input_sig_index, false);
  gpio_iomux_in(_step_pin, mapping->input_sig_index);
}

void StepperQueue::disconnect() {
  // sig_index = 0x100 => cancel output
  gpio_matrix_out(_step_pin, 0x100, false, false);
  // untested alternative:
  //	gpio_reset_pin((gpio_num_t)q->step_pin);
}

// Mechanism is like this, starting from stopped motor:
//
// *	init counter
// *	init mcpwm
// *	start mcpwm
// *	-- pcnt counter counts every L->H-transition at mcpwm.timer = 1
// *	-- if counter reaches planned steps, then counter is reset and
// *	interrupt is created
//
// *	pcnt interrupt: available time is from mcpwm.timer = 1+x to period
//		-	read next commmand: store period in counter shadow and
// steps in pcnt
//		- 	without next command: set mcpwm to stop mode on reaching
// period

bool StepperQueue::isRunning() {
  if (_hasISRactive) {
    return true;
  }
  mcpwm_unit_t mcpwm_unit = mapping->mcpwm_unit;
  mcpwm_dev_t *mcpwm = mcpwm_unit == MCPWM_UNIT_0 ? &MCPWM0 : &MCPWM1;
  uint8_t timer = mapping->timer;
  if (mcpwm->timer[timer].status.value > 1) {
    return true;
  }
  return (mcpwm->timer[timer].mode.start == 2);  // 2=run continuous
}

void StepperQueue::commandAddedToQueue(bool start) {
#ifdef TEST_PROBE
  // The time used by this command can have an impact
  digitalWrite(TEST_PROBE, digitalRead(TEST_PROBE) == HIGH ? LOW : HIGH);
#endif
  noInterrupts();
  bool first = (next_write_idx++ == read_idx);
  if (_hasISRactive) {
    interrupts();
    return;
  }
  interrupts();

  // If it is not the first command in the queue, then just return
  // Otherwise just prepare, what is possible for start (set direction pin)
  if (!first && !start) {
    return;
  }

  mcpwm_unit_t mcpwm_unit = mapping->mcpwm_unit;
  mcpwm_dev_t *mcpwm = mcpwm_unit == MCPWM_UNIT_0 ? &MCPWM0 : &MCPWM1;
  uint8_t timer = mapping->timer;

  _hasISRactive = true;
  struct queue_entry *e = &entry[read_idx++ & QUEUE_LEN_MASK];
  apply_command(this, e);

  if (start) {
    mcpwm->timer[timer].mode.start = 2;  // 2=run continuous
  }
}
int8_t StepperQueue::startPreparedQueue() {
  if (next_write_idx == read_idx) {
    return AQE_ERROR_EMPTY_QUEUE_TO_START;
  }
  uint8_t timer = mapping->timer;
  mcpwm_unit_t mcpwm_unit = mapping->mcpwm_unit;
  mcpwm_dev_t *mcpwm = mcpwm_unit == MCPWM_UNIT_0 ? &MCPWM0 : &MCPWM1;
  mcpwm->timer[timer].mode.start = 2;  // 2=run continuous
  return AQE_OK;
}
void StepperQueue::forceStop() {
  init_stop(this);
  read_idx = next_write_idx;
}
bool StepperQueue::isValidStepPin(uint8_t step_pin) { return true; }
int8_t StepperQueue::queueNumForStepPin(uint8_t step_pin) { return -1; }

void _esp32_attachToPulseCounter(uint8_t pcnt_unit, FastAccelStepper *stepper) {
  pcnt_config_t cfg;
  uint8_t dir_pin = stepper->getDirectionPin();
  cfg.pulse_gpio_num = stepper->getStepPin();
  if (dir_pin == PIN_UNDEFINED) {
    cfg.ctrl_gpio_num = PCNT_PIN_NOT_USED;
  } else {
    cfg.ctrl_gpio_num = dir_pin;
  }
  if (stepper->directionPinHighCountsUp()) {
    cfg.lctrl_mode = PCNT_MODE_REVERSE;
    cfg.hctrl_mode = PCNT_MODE_KEEP;
  } else {
    cfg.lctrl_mode = PCNT_MODE_KEEP;
    cfg.hctrl_mode = PCNT_MODE_REVERSE;
  }
  cfg.pos_mode = PCNT_COUNT_INC;  // increment on rising edge
  cfg.neg_mode = PCNT_COUNT_DIS;  // ignore falling edge
  cfg.counter_h_lim = 16384;
  cfg.counter_l_lim = -16384;
  cfg.unit = (pcnt_unit_t)pcnt_unit;
  cfg.channel = PCNT_CHANNEL_0;
  pcnt_unit_config(&cfg);

  PCNT.conf_unit[cfg.unit].conf0.thr_h_lim_en = 0;
  PCNT.conf_unit[cfg.unit].conf0.thr_l_lim_en = 0;

  pcnt_counter_clear(cfg.unit);
  pcnt_counter_resume(cfg.unit);

  stepper->detachFromPin();
  stepper->reAttachToPin();
  gpio_iomux_in(stepper->getStepPin(), PCNT_SIG_CH0_IN7_IDX);
  if (dir_pin != PIN_UNDEFINED) {
    gpio_matrix_out(stepper->getDirectionPin(), 0x100, false, false);
    gpio_iomux_in(stepper->getDirectionPin(), PCNT_CTRL_CH0_IN7_IDX);
    pinMode(stepper->getDirectionPin(), OUTPUT);
  }
}
int16_t _esp32_readPulseCounter(uint8_t pcnt_unit) {
  return PCNT.cnt_unit[pcnt_unit].cnt_val;
}
#endif
