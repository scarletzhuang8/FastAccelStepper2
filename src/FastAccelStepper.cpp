#include "FastAccelStepper.h"

#include "StepperISR.h"

// This define in order to not shoot myself.
#ifndef TEST
#define printf DO_NOT_USE_PRINTF
#endif

// Here are the global variables to interface with the interrupts

// To realize the 1 Hz debug led
uint8_t fas_ledPin = 255;  // 255 if led blinking off
uint16_t fas_debug_led_cnt = 0;

#define TIMER_FREQ F_CPU
#if (TIMER_FREQ == 16000000)
#define UPM_TIMER_FREQ ((upm_float)0x97f4)
#else
upm_float upm_timer_freq;
#define UPM_TIMER_FREQ upm_timer_freq
#endif

#if defined(ARDUINO_ARCH_AVR)
FastAccelStepperEngine *fas_engine = NULL;

// dynamic allocation seem to not work so well
FastAccelStepper fas_stepperA = FastAccelStepper();
FastAccelStepper fas_stepperB = FastAccelStepper();
#endif

//*************************************************************************************************
//*************************************************************************************************
//
// Main purpose of FastAccelStepperEngine is timer 1 initialization and access
// to the steppers.
//
//*************************************************************************************************
//*************************************************************************************************
void FastAccelStepperEngine::init() {
#if (TIMER_FREQ != 16000000)
  upm_timer_freq = upm_from((uint32_t)TIMER_FREQ);
#endif
#if defined(ARDUINO_ARCH_AVR)
  fas_engine = this;

  // Initialize timer for stepper background task and correct time base
  noInterrupts();

  // Set WGM13:0 to all zero => Normal mode
  TCCR1A &= ~(_BV(WGM11) | _BV(WGM10));
  TCCR1B &= ~(_BV(WGM13) | _BV(WGM12));

  // Set prescaler to 1
  TCCR1B = (TCCR1B & ~(_BV(CS12) | _BV(CS11) | _BV(CS10))) | _BV(CS10);

  // enable OVF interrupt
  TIMSK1 |= _BV(TOIE1);

  interrupts();
#endif
}
//*************************************************************************************************
FastAccelStepper* FastAccelStepperEngine::stepperConnectToPin(uint8_t step_pin) {
  uint8_t i;
  for (i = 0; i < MAX_STEPPER; i++) {
    FastAccelStepper* s = _stepper[i];
    if (s) {
      if (s->getStepPin() == step_pin) {
        return NULL;
      }
    }
  }
#if defined(ARDUINO_ARCH_AVR)
	FastAccelStepper* s;
  if (step_pin == stepPinStepperA) {
	s = &fas_stepperA;
  }
  else if (step_pin == stepPinStepperB) {
	s = &fas_stepperB;
  }
  else {
	return NULL;
  }

	_stepper[_next_stepper_num] = s;
	s->init(_next_stepper_num, step_pin);
	_next_stepper_num++;
    return s;
#endif
#if defined(ARDUINO_ARCH_ESP32)
  return NULL;
#endif
}
//*************************************************************************************************
void FastAccelStepperEngine::setDebugLed(uint8_t ledPin) {
  fas_ledPin = ledPin;
}
//*************************************************************************************************
void FastAccelStepperEngine::manageSteppers() {
  if (fas_ledPin < 255) {
    fas_debug_led_cnt++;
    if (fas_debug_led_cnt == 144) {
      digitalWrite(fas_ledPin, HIGH);
    }
    if (fas_debug_led_cnt == 288) {
      digitalWrite(fas_ledPin, LOW);
      fas_debug_led_cnt = 0;
    }
  }
  for (uint8_t i = 0; i < _next_stepper_num;i++) {
	 FastAccelStepper* s = _stepper[i];
	if (s) {
	  s->isr_fill_queue();
	}
  }
}


//*************************************************************************************************
//*************************************************************************************************
//
// FastAccelStepper is associated with either stepperA or stepperB and provides:
// - movement control
//       either raw access to the stepper command queue
//       or ramp generator driven by speed/acceleration and move
// - stepper position
//
//*************************************************************************************************
//*************************************************************************************************

//*************************************************************************************************
bool FastAccelStepper::isStopped() { return _ticks_at_queue_end == 0; }
//*************************************************************************************************
void FastAccelStepper::addQueueStepperStop() { _ticks_at_queue_end = 0; }
//*************************************************************************************************
int FastAccelStepper::addQueueEntry(uint32_t start_delta_ticks, uint8_t steps,
                                    bool dir_high, int16_t change_ticks) {
  int32_t c_sum = 0;
  if (steps >= 128) {
    return AQE_STEPS_ERROR;
  }
  if (start_delta_ticks > ABSOLUTE_MAX_TICKS) {
    return AQE_TOO_HIGH;
  }
  if ((change_ticks != 0) && (steps > 1)) {
    c_sum = change_ticks * (steps - 1);
  }
  if (change_ticks > 0) {
    if (c_sum > 32768) {
      return AQE_CHANGE_TOO_HIGH;
    }
  } else if (change_ticks < 0) {
    if (c_sum < -32768) {
      return AQE_CHANGE_TOO_LOW;
    }
    if (start_delta_ticks + c_sum < MIN_DELTA_TICKS) {
      return AQE_CUMULATED_CHANGE_TOO_LOW;
    }
  }

  uint16_t msb = start_delta_ticks >> 14;
  uint16_t lsw;
  if (msb > 1) {
    msb--;
    lsw = start_delta_ticks & 0x3fff;
    lsw |= 0x4000;
  } else {
    msb = 0;
    lsw = start_delta_ticks;
  }

  uint8_t wp = fas_queue[_stepper_num].next_write_ptr;
  uint8_t rp = fas_queue[_stepper_num].read_ptr;
  struct queue_entry* e = &fas_queue[_stepper_num].entry[wp];

  uint8_t next_wp = (wp + 1) & QUEUE_LEN_MASK;
  if (next_wp != rp) {
    _pos_at_queue_end += dir_high ? steps : -steps;
    _ticks_at_queue_end = change_ticks * (steps - 1) + start_delta_ticks;
    steps <<= 1;
    e->delta_msb = msb;
    e->delta_lsw = lsw;
    e->delta_change = change_ticks;
    e->steps = (dir_high != _dir_high_at_queue_end) ? steps | 0x01 : steps;
    _dir_high_at_queue_end = dir_high;
#if (TEST_CREATE_QUEUE_CHECKSUM == 1)
    {
      unsigned char* x = (unsigned char*)e;
      for (uint8_t i = 0; i < sizeof(struct queue_entry); i++) {
        if (checksum & 0x80) {
          checksum <<= 1;
          checksum ^= 0xde;
        } else {
          checksum <<= 1;
        }
        checksum ^= *x++;
      }
    }
#endif
    fas_queue[_stepper_num].next_write_ptr = next_wp;
    return AQE_OK;
  }
  return AQE_FULL;
}

//*************************************************************************************************
// fill_queue generates commands to the stepper for executing a ramp
//
// Plan is to fill the queue with commmands with approx. 10 ms ahead (or more).
// For low speeds, this results in single stepping
// For high speeds (40kSteps/s) approx. 400 Steps to be created using 3 commands
//
// Basis of the calculation is the relation between steps and time via
// acceleration a:
//
//		s = 1/2 * a * t²
//
// With v = a * t for the acceleration case, then v can be deducted:
//
//		s = 1/2 * v² / a
//
//	    v = sqrt(2 * s * a)
//
//*************************************************************************************************
void FastAccelStepper::_calculate_move(int32_t move) {
	Serial.print("QUEUE=");
	Serial.print(_stepper_num);
	Serial.print(" ");
	Serial.println(_queue_num);
#if (TEST_CREATE_QUEUE_CHECKSUM == 1)
  checksum = 0;
#endif
  if (move == 0) {
    return;
  }
  if ((move < 0) && (_dirPin == 255)) {
    return;
  }
  uint32_t steps = abs(move);

  uint32_t curr_ticks = _ticks_at_queue_end;
  uint32_t performed_ramp_up_steps;
  uint32_t deceleration_start;
  uint32_t upm_sq_min_travel;
  uint32_t ramp_steps = _ramp_steps;
  if ((curr_ticks == 0) || isQueueEmpty()) {
    // motor is not running
    //
    // ramp starts with s = 0.
    //
    performed_ramp_up_steps = 0;

    // If the maximum speed cannot be reached due to too less steps,
    // then in the isr_single_fill_queue routine the deceleration will be
    // started after move/2 steps.
    deceleration_start = min(ramp_steps, steps / 2);
  } else if (curr_ticks == _min_travel_ticks) {
    // motor is running already at coast speed
    //
    performed_ramp_up_steps = ramp_steps;
    deceleration_start = ramp_steps;
  } else {
    uint32_t on_ramp_steps;
    // motor is running
    //
    // Calculate on which step on the speed ramp the current speed is related to
    performed_ramp_up_steps =
        upm_to_u32(divide(_upm_inv_accel2, square(upm_from(curr_ticks))));
    if (curr_ticks >= _min_travel_ticks) {
      // possibly can speed up
      // Full ramp up/down needs 2*ramp_steps
      // => full ramp is possible, if move+performed_ramp_up_steps >
      // 2*ramp_steps
      deceleration_start =
          min(ramp_steps, (move + performed_ramp_up_steps) / 2);
    } else if (curr_ticks < _min_travel_ticks) {
      // speed too high, so need to reduce to _min_travel_ticks speed
      deceleration_start = ramp_steps;
    }
  }

  noInterrupts();
  _deceleration_start = deceleration_start;
  _performed_ramp_up_steps = performed_ramp_up_steps;
  isr_speed_control_enabled = true;
  interrupts();

#ifdef TEST
  printf(
      "Ramp data: steps to move = %d  curr_ticks = %d travel_ticks = %d "
      "Ramp steps = %d Performed ramp steps = %d deceleration start = %u\n",
      steps, curr_ticks, _min_travel_ticks, ramp_steps, performed_ramp_up_steps,
      deceleration_start);
#endif
#ifdef DEBUG
  char buf[256];
  sprintf(
      buf,
      "Ramp data: steps to move = %ld  curr_ticks = %ld travel_ticks = %ld "
      "Ramp steps = %ld Performed ramp steps = %ld deceleration start = %lu\n",
      steps, curr_ticks, _min_travel_ticks, ramp_steps, performed_ramp_up_steps,
      deceleration_start);
  Serial.println(buf);
#endif
}

inline void FastAccelStepper::isr_fill_queue() {
  // Check preconditions to be allowed to fill the queue
  if (target_pos == _pos_at_queue_end) {
    isr_speed_control_enabled = false;
    return;
  }
  if (_min_travel_ticks == 0) {
    return;
  }

  // preconditions are fulfilled, so create the command(s)
  while (!isQueueFull() && isr_speed_control_enabled) {
    isr_single_fill_queue();
  }
}

inline void FastAccelStepper::isr_single_fill_queue() {
#if (TEST_MEASURE_ISR_SINGLE_FILL == 1)
  // For run time measurement
  uint32_t runtime_us = micros();
#endif

  // check state for acceleration/deceleration or deceleration to stop
  uint32_t remaining_steps = abs(target_pos - _pos_at_queue_end);
  uint32_t planning_steps;
  uint32_t next_ticks;
  if (ramp_state == RAMP_STATE_IDLE) {  // motor is stopped. Set to max value
    planning_steps = 1;
    ramp_state = RAMP_STATE_ACCELERATE;
  } else {
    planning_steps = max(16000 / _ticks_at_queue_end, 1);
    if (remaining_steps <= _deceleration_start) {
      ramp_state = RAMP_STATE_DECELERATE_TO_STOP;
    } else if (_min_travel_ticks < _ticks_at_queue_end) {
      ramp_state = RAMP_STATE_ACCELERATE;
    } else if (_min_travel_ticks > _ticks_at_queue_end) {
      ramp_state = RAMP_STATE_DECELERATE;
    } else {
      ramp_state = RAMP_STATE_COAST;
    }
  }

  // Forward planning of minimum 10ms or more on slow speed.

#ifdef TEST
  printf("remaining=%u deceleration_start=%u planning steps=%d   ",
         remaining_steps, _deceleration_start, planning_steps);
  switch (ramp_state) {
    case RAMP_STATE_COAST:
      printf("COAST");
      break;
    case RAMP_STATE_ACCELERATE:
      printf("ACC");
      break;
    case RAMP_STATE_DECELERATE:
      printf("DEC");
      break;
    case RAMP_STATE_DECELERATE_TO_STOP:
      printf("STOP");
      break;
  }
  printf("\n");
#endif

  uint32_t curr_ticks = _ticks_at_queue_end;
#ifdef TEST
  float v2;
#endif
  switch (ramp_state) {
    uint32_t d_ticks_new;
    uint32_t upm_rem_steps;
    upm_float upm_d_ticks_new;
    case RAMP_STATE_COAST:
      next_ticks = _min_travel_ticks;
      // do not overshoot ramp down start
      planning_steps =
          min(planning_steps, remaining_steps - _deceleration_start);
      break;
    case RAMP_STATE_ACCELERATE:
      upm_rem_steps = upm_from(_performed_ramp_up_steps + planning_steps);
      upm_d_ticks_new = sqrt(divide(_upm_inv_accel2, upm_rem_steps));

      d_ticks_new = upm_to_u32(upm_d_ticks_new);

      // avoid overshoot
      next_ticks = max(d_ticks_new, _min_travel_ticks);
      if (_performed_ramp_up_steps == 0) {
        curr_ticks = d_ticks_new;
      } else {
        // CLIPPING: avoid increase
        next_ticks = min(next_ticks, curr_ticks);
      }

#ifdef TEST
      printf("accelerate ticks => %d  during %d ticks (d_ticks_new = %u)\n",
             next_ticks, planning_steps, d_ticks_new);
      printf("... %u+%u steps\n", _performed_ramp_up_steps, planning_steps);
#endif
      break;
    case RAMP_STATE_DECELERATE:
      upm_rem_steps = upm_from(_performed_ramp_up_steps + planning_steps);
      upm_d_ticks_new = sqrt(divide(_upm_inv_accel2, upm_rem_steps));

      d_ticks_new = upm_to_u32(upm_d_ticks_new);

      // avoid undershoot
      next_ticks = min(d_ticks_new, _min_travel_ticks);

      // CLIPPING: avoid reduction
      next_ticks = max(next_ticks, curr_ticks);

#ifdef TEST
      printf("decelerate ticks => %d  during %d ticks (d_ticks_new = %u)\n",
             next_ticks, planning_steps, d_ticks_new);
      printf("... %u+%u steps\n", _performed_ramp_up_steps, planning_steps);
#endif
      break;
    case RAMP_STATE_DECELERATE_TO_STOP:
      upm_rem_steps = upm_from(remaining_steps - planning_steps);
      upm_d_ticks_new = sqrt(divide(_upm_inv_accel2, upm_rem_steps));

      d_ticks_new = upm_to_u32(upm_d_ticks_new);

      // avoid undershoot
      next_ticks = max(d_ticks_new, _min_travel_ticks);

      // CLIPPING: avoid reduction
      next_ticks = max(next_ticks, curr_ticks);
#ifdef TEST
      printf("decelerate ticks => %d  during %d ticks (d_ticks_new = %u)\n",
             next_ticks, planning_steps, d_ticks_new);
#endif
  }

  // CLIPPING: avoid increase
  next_ticks = min(next_ticks, ABSOLUTE_MAX_TICKS);

  // Number of steps to execute with limitation to min 1 and max remaining steps
  uint16_t total_steps = planning_steps;
#ifdef TEST
  printf(
      "total_steps for the command = %d  with planning_steps = %u and "
      "next_ticks = %u\n",
      total_steps, planning_steps, next_ticks);
#endif
  total_steps = max(total_steps, 1);
  total_steps = min(total_steps, abs(remaining_steps));
  uint16_t steps = total_steps;

  // Calculate change per step
  int32_t total_change = (int32_t)next_ticks - (int32_t)curr_ticks;
  int32_t change = total_change;
  if (steps > 1) {
    change /= steps;  // each step will change
  }

  // Number of commands
  uint8_t command_cnt =
      min(steps, max(steps / 128 + 1, (abs(total_change) + 32767) / 32768));

  // Steps per command
  uint16_t steps_per_command = (steps + command_cnt - 1) / command_cnt;

  if (steps_per_command * command_cnt > steps) {
    steps_per_command -= 1;
  }

  if (ramp_state == RAMP_STATE_ACCELERATE) {
    _performed_ramp_up_steps += steps;
  }
  if (ramp_state == RAMP_STATE_DECELERATE) {
    _performed_ramp_up_steps -= steps;
  }
  // Apply change to curr_ticks
  if (change == 0) {
    curr_ticks += total_change;
  } else {
    curr_ticks += change;
  }

  bool dir = target_pos > _pos_at_queue_end;

#ifdef TEST
  if (steps > 1) {
    printf(
        "Issue %d commands for %d steps with %d steps/command for total "
        "change %d and %d change/step and start with %u ticks\n",
        command_cnt, steps, steps_per_command, total_change, change,
        curr_ticks);
  } else {
    printf(
        "Issue %d command for %d steps "
        "and start with %u ticks\n",
        command_cnt, steps, curr_ticks);
  }
#endif

  uint32_t sum_dt = 0;
  for (uint16_t c = 1; c < command_cnt; c++) {
    int8_t res = addQueueEntry(curr_ticks, steps_per_command, dir, change);
#ifdef TEST
    printf(
        "add command %d Steps = %d start_ticks = %d  Target "
        "pos = %d "
        "Remaining steps = %d  tick_change=%d"
        " => res=%d   ticks_at_queue_end = %d\n",
        (command_cnt + 1 - c), steps_per_command, curr_ticks, target_pos,
        remaining_steps, change, res, _ticks_at_queue_end);
#endif
    curr_ticks += steps_per_command * change;
    steps -= steps_per_command;
  }
  int8_t res = addQueueEntry(curr_ticks, steps, dir, change);
#ifdef TEST
  printf(
      "add command Steps = %d start_ticks = %d  Target "
      "pos = %d "
      "Remaining steps = %d  tick_change=%d"
      " => res=%d   ticks_at_queue_end = %d\n",
      steps, curr_ticks, target_pos, remaining_steps, change, res,
      _ticks_at_queue_end);
#endif
  if (res != 0) {  // Emergency stop on internal error
    addQueueStepperStop();
    ramp_state = RAMP_STATE_IDLE;
    isr_speed_control_enabled = false;
  }
  if (total_steps == abs(remaining_steps)) {
    addQueueStepperStop();
    ramp_state = RAMP_STATE_IDLE;
    isr_speed_control_enabled = false;
#ifdef TEST
    puts("Stepper stop");
#endif
  }

#ifdef TEST
  puts("");
#endif

#if (TEST_MEASURE_ISR_SINGLE_FILL == 1)
  // For run time measurement
  runtime_us = micros() - runtime_us;
  max_micros = max(max_micros, runtime_us);
#endif
}
#if defined(ARDUINO_ARCH_AVR)
ISR(TIMER1_OVF_vect) {
  // disable OVF interrupt to avoid nesting
  TIMSK1 &= ~_BV(TOIE1);

  // enable interrupts for nesting
  interrupts();

  // manage steppers
  fas_engine->manageSteppers();

  // enable OVF interrupt again
  TIMSK1 |= _BV(TOIE1);
}
#endif

void FastAccelStepper::init(uint8_t num, uint8_t step_pin) {
#if (TEST_MEASURE_ISR_SINGLE_FILL == 1)
  // For run time measurement
  max_micros = 0;
#endif
#if (TEST_CREATE_QUEUE_CHECKSUM == 1)
  checksum = 0;
#endif

  target_pos = 0;
  isr_speed_control_enabled = false;
  ramp_state = RAMP_STATE_IDLE;
  _stepper_num = num;
  _auto_enablePin = 255;
  _pos_at_queue_end = 0;
  _dir_high_at_queue_end = true;
  _min_travel_ticks = 0;
  _ticks_at_queue_end = 0;
  _stepPin = step_pin;

#if defined(ARDUINO_ARCH_AVR)
  // start interrupt
  _queue_num = step_pin == stepPinStepperA ? 0 : 1;
  fas_queue[_queue_num].init(step_pin);
#endif
}
uint8_t FastAccelStepper::getStepPin() { return _stepPin; }
void FastAccelStepper::setDirectionPin(uint8_t dirPin) {
  _dirPin = dirPin;
  digitalWrite(dirPin, HIGH);
  pinMode(dirPin, OUTPUT);
  fas_queue[_stepper_num].dirPin = dirPin;
}
void FastAccelStepper::setEnablePin(uint8_t enablePin) {
  _enablePin = enablePin;
  digitalWrite(enablePin, HIGH);
  pinMode(enablePin, OUTPUT);
}
void FastAccelStepper::setAutoEnable(bool auto_enable) {
  if (auto_enable) {
    _auto_enablePin = _enablePin;
  } else {
    _auto_enablePin = 255;
  }
  fas_queue[_stepper_num].autoEnablePin = _auto_enablePin;
}
void FastAccelStepper::setSpeed(uint32_t min_step_us) {
  _min_travel_ticks = min_step_us * (TIMER_FREQ / 1000L) / 1000L;
  _update_ramp_steps();
}
void FastAccelStepper::setAcceleration(uint32_t accel) {
  uint32_t tmp = TIMER_FREQ / 2;
  upm_float upm_inv_accel = upm_from(tmp / accel);
  _upm_inv_accel2 = multiply(UPM_TIMER_FREQ, upm_inv_accel);
  _update_ramp_steps();
}
void FastAccelStepper::_update_ramp_steps() {
  _ramp_steps =
      upm_to_u32(divide(_upm_inv_accel2, square(upm_from(_min_travel_ticks))));
}
void FastAccelStepper::move(int32_t move) {
  target_pos = _pos_at_queue_end + move;
  _calculate_move(move);
}
void FastAccelStepper::moveTo(int32_t position) {
  int32_t move;
  target_pos = position;
  move = position - _pos_at_queue_end;
  _calculate_move(move);
}
void FastAccelStepper::disableOutputs() {
  if (_enablePin != 255) {
    digitalWrite(_enablePin, HIGH);
  }
}
void FastAccelStepper::enableOutputs() {
  if (_enablePin != 255) {
    digitalWrite(_enablePin, LOW);
  }
}
int32_t FastAccelStepper::getPositionAfterCommandsCompleted() {
  return _pos_at_queue_end;
}
int32_t FastAccelStepper::getCurrentPosition() {
  int32_t pos = _pos_at_queue_end;
  bool dir = _dir_high_at_queue_end;
  struct queue_entry* q;
  noInterrupts();
  uint8_t wp = fas_queue[_stepper_num].next_write_ptr;
  uint8_t rp = fas_queue[_stepper_num].read_ptr;
  struct queue_entry* e = &fas_queue[_stepper_num].entry[wp];
  interrupts();
  if (rp != wp) {
    while (rp != wp) {
      wp = (wp + QUEUE_LEN - 1) & QUEUE_LEN_MASK;
      uint8_t steps = q[wp].steps;
      if (dir) {
        pos -= steps >> 1;
      } else {
        pos += steps >> 1;
      }
      if (steps & 1) {
        dir = !dir;
      }
    }
  }
  return pos;
}
bool FastAccelStepper::isQueueFull() {
  StepperQueue* q = &fas_queue[_stepper_num];
  return (((q->next_write_ptr + 1) & QUEUE_LEN_MASK) == q->read_ptr);
}
bool FastAccelStepper::isQueueEmpty() {
  StepperQueue* q = &fas_queue[_stepper_num];
  return (q->read_ptr == q->next_write_ptr);
}
bool FastAccelStepper::isRunning() { return !isQueueEmpty(); }
