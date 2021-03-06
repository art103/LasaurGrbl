/*
  planner.c - buffers movement commands and manages the acceleration profile plan
  Part of LasaurGrbl

  Copyright (c) 2009-2011 Simen Svale Skogsrud
  Copyright (c) 2011 Sungeun K. Jeon
  Copyright (c) 2011 Stefan Hechenberger

  LasaurGrbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  LasaurGrbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
*/

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "planner.h"
#include "stepper.h"
#include "sense_control.h"
#include "config.h"


// The number of linear motions that can be in the plan at any give time
#define BLOCK_BUFFER_SIZE 48  // do not make bigger than uint8_t
#define NUM_RASTERS 3

static block_t block_buffer[BLOCK_BUFFER_SIZE];  // ring buffer for motion instructions
static volatile uint8_t block_buffer_head;       // index of the next block to be pushed
static volatile uint8_t block_buffer_tail;       // index of the block to process now
static volatile uint8_t block_buffer_tail_write;
static volatile uint8_t block_buffers_used;

// Ring buffer used for raster data.
static uint8_t raster_buffer[NUM_RASTERS][RASTER_BUFFER_SIZE];
static volatile uint8_t raster_buffer_next = 0;
static volatile uint8_t raster_buffer_count = 0;

static int32_t position[3];             // The current position of the tool in absolute steps
static volatile bool position_update_requested;  // make sure to update to stepper position on next occasion
static double previous_unit_vec[3];     // Unit vector of previous path line segment
static double previous_nominal_speed;   // Nominal speed of previous path line segment

// prototypes for static functions (non-accesible from other files)
static int8_t next_block_index(int8_t block_index);
static int8_t prev_block_index(int8_t block_index);
static double estimate_acceleration_distance(double initial_rate, double target_rate, double acceleration);
static double intersection_distance(double initial_rate, double final_rate, double acceleration, double distance);
static double max_allowable_speed(double acceleration, double target_velocity, double distance);
static void calculate_trapezoid_for_block(block_t *block, double entry_factor, double exit_factor);
static void reduce_entry_speed_reverse(block_t *current, block_t *next);
static void reduce_entry_speed_forward(block_t *previous, block_t *current);
static void planner_recalculate();


// Add a new linear movement to the buffer. x, y and z is 
// the signed, absolute target position in millimeters. Feed rate specifies the speed of the motion.
static void planner_movement(double x, double y, double z,
                      double feed_rate, double acceleration,
                      uint8_t nominal_laser_intensity, uint16_t ppi,
                      raster_t *raster) {
  // calculate target position in absolute steps
  int32_t target[3];

  if (sense_ignore == 0) {
      // Make sure we stay within our limits
    #if defined(CONFIG_X_MIN)
      x=max(x, CONFIG_X_MIN);
    #endif
    #if defined(CONFIG_X_MAX)
      x=min(x, CONFIG_X_MAX);
    #endif
    #if defined(CONFIG_Y_MIN)
      y=max(y, CONFIG_Y_MIN);
    #endif
    #if defined(CONFIG_Y_MAX)
      y=min(y, CONFIG_Y_MAX);
    #endif
    #if defined(CONFIG_Z_MIN)
      z=max(z, CONFIG_Z_MIN);
    #endif
    #if defined(CONFIG_Z_MAX)
      z=min(z, CONFIG_Z_MAX);
    #endif
  }

  target[X_AXIS] = lround(x*x_steps_per_mm);
  target[Y_AXIS] = lround(y*y_steps_per_mm);
  target[Z_AXIS] = lround(z*CONFIG_Z_STEPS_PER_MM); 

  // calculate the buffer head and check for space
  int next_buffer_head = next_block_index( block_buffer_head ); 
  while(block_buffer_tail == next_buffer_head) {  // buffer full condition
    // good! We are well ahead of the robot. Rest here until buffer has room.
    // sleep_mode();
  }
  
  block_buffers_used++;

  // handle position update after a stop
  if (position_update_requested) {
    planner_set_position(stepper_get_position_x(), stepper_get_position_y(), stepper_get_position_z());
    position_update_requested = false;
    //printString("planner pos update\n");  // debug
  }
  
  // prepare to set up new block
  block_t *block = &block_buffer[block_buffer_head];
  
  // Setup the block type
  if (raster == NULL) {
      block->block_type = BLOCK_TYPE_LINE;
  } else {
      block->block_type = BLOCK_TYPE_RASTER_LINE;
      memcpy(&block->raster, raster, sizeof(raster_t));
  }

  // set nominal laser intensity
  block->laser_pwm = nominal_laser_intensity;

  // compute direction bits for this block
  block->direction_bits = 0;
  if (target[X_AXIS] < position[X_AXIS]) { block->direction_bits |= (1<<STEP_X_DIR); }
  if (target[Y_AXIS] < position[Y_AXIS]) { block->direction_bits |= (1<<STEP_Y_DIR); }
#ifndef MOTOR_Z
  if (target[Z_AXIS] < position[Z_AXIS]) { block->direction_bits |= (1<<STEP_Z_DIR); }
#else
  if (target[Z_AXIS] < position[Z_AXIS]) { block->direction_bits |= STEP_Z_MASK; }
#endif
  
  // number of steps for each axis
  block->steps_x = labs(target[X_AXIS]-position[X_AXIS]);
  block->steps_y = labs(target[Y_AXIS]-position[Y_AXIS]);
  block->steps_z = labs(target[Z_AXIS]-position[Z_AXIS]);
  block->step_event_count = max(block->steps_x, max(block->steps_y, block->steps_z));
  if (block->step_event_count == 0) { return; };  // bail if this is a zero-length block
  
  // compute path vector in terms of absolute step target and current positions
  double delta_mm[3];
  delta_mm[X_AXIS] = (target[X_AXIS]-position[X_AXIS])/x_steps_per_mm;
  delta_mm[Y_AXIS] = (target[Y_AXIS]-position[Y_AXIS])/y_steps_per_mm;
  delta_mm[Z_AXIS] = (target[Z_AXIS]-position[Z_AXIS])/CONFIG_Z_STEPS_PER_MM;
  block->millimeters = sqrt( (delta_mm[X_AXIS]*delta_mm[X_AXIS]) + 
                             (delta_mm[Y_AXIS]*delta_mm[Y_AXIS]) + 
                             (delta_mm[Z_AXIS]*delta_mm[Z_AXIS]) );
  double inverse_millimeters = 1.0/block->millimeters;  // store for efficency  
  
  // calculate nominal_speed (mm/min) and nominal_rate (step/min)
  // minimum stepper speed is limited by MINIMUM_STEPS_PER_MINUTE in stepper.c
  block->nominal_speed = feed_rate; // always > 0
  block->nominal_rate = ceil(feed_rate * x_steps_per_mm); // always > 0

  block->acceleration = acceleration;
  // compute the acceleration rate for this block. (steps/min/min / ticks/min)
  block->rate_delta = ceil( block->acceleration * CONFIG_X_STEPS_PER_MM / (ACCELERATION_TICKS_PER_SECOND * 60));

  // Calculate the ppi steps
  block->laser_mmpp = 0;
  block->laser_ppi = 0;
  if (ppi > 0) {
      block->laser_ppi = ppi; // Only used by LCD output.
      block->laser_mmpp = MM_PER_INCH / ppi;
  }

  //// acceleeration manager calculations
  // Compute path unit vector                            
  double unit_vec[3];
  unit_vec[X_AXIS] = delta_mm[X_AXIS]*inverse_millimeters;
  unit_vec[Y_AXIS] = delta_mm[Y_AXIS]*inverse_millimeters;
  unit_vec[Z_AXIS] = delta_mm[Z_AXIS]*inverse_millimeters;  

  // Compute max junction speed by centripetal acceleration approximation.
  // Let a circle be tangent to both previous and current path line segments, where the junction 
  // deviation is defined as the distance from the junction to the closest edge of the circle, 
  // colinear with the circle center. The circular segment joining the two paths represents the 
  // path of centripetal acceleration. Solve for max velocity based on max acceleration about the
  // radius of the circle, defined indirectly by junction deviation. This may be also viewed as 
  // path width or max_jerk in the previous grbl version. This approach does not actually deviate 
  // from path, but used as a robust way to compute cornering speeds, as it takes into account the
  // nonlinearities of both the junction angle and junction velocity.
  double vmax_junction = ZERO_SPEED; // prime for junctions close to 0 degree
  if ((block_buffer_head != block_buffer_tail) && (previous_nominal_speed > 0.0)) {
    // Compute cosine of angle between previous and current path.
    // vmax_junction is computed without sin() or acos() by trig half angle identity.
    double cos_theta = - previous_unit_vec[X_AXIS] * unit_vec[X_AXIS] 
                       - previous_unit_vec[Y_AXIS] * unit_vec[Y_AXIS] 
                       - previous_unit_vec[Z_AXIS] * unit_vec[Z_AXIS] ;
    if (cos_theta < 0.95) {
      // any junction *not* close to 0 degree
      vmax_junction = min(previous_nominal_speed, block->nominal_speed);  // prime for close to 180
      if (cos_theta > -0.95) {
        // any junction not close to neither 0 and 180 degree -> compute vmax
        double sin_theta_d2 = sqrt(0.5*(1.0-cos_theta)); // Trig half angle identity. Always positive.
        vmax_junction = min( vmax_junction, sqrt( block->acceleration * CONFIG_JUNCTION_DEVIATION
                                                  * sin_theta_d2/(1.0-sin_theta_d2) ) );
      }
    }
  }
  block->vmax_junction = vmax_junction;
  
  // Initialize entry_speed. Compute based on deceleration to zero.
  // This will be updated in the forward and reverse planner passes.
  double v_allowable = max_allowable_speed(-block->acceleration, ZERO_SPEED, block->millimeters);
  block->entry_speed = min(vmax_junction, v_allowable);

  // Set nominal_length_flag for more efficiency.
  // If a block can de/ac-celerate from nominal speed to zero within the length of 
  // the block, then the speed will always be at the the maximum junction speed and 
  // may always be ignored for any speed reduction checks.
  if (block->nominal_speed <= v_allowable) { block->nominal_length_flag = true; }
  else { block->nominal_length_flag = false; }
  block->recalculate_flag = true; // always calculate trapezoid for new block

  // update previous unit_vector and nominal speed
  memcpy(previous_unit_vec, unit_vec, sizeof(unit_vec)); // previous_unit_vec[] = unit_vec[]
  previous_nominal_speed = block->nominal_speed;
  //// end of acceleeration manager calculations


  // move buffer head and update position
  block_buffer_head = next_buffer_head;     
  memcpy(position, target, sizeof(target)); // position[] = target[]

  planner_recalculate();

  // make sure the stepper interrupt is processing
  stepper_wake_up();
}

void planner_init() {
  block_buffer_head = 0;
  block_buffer_tail = 0;
  block_buffer_tail_write = 0;
  raster_buffer_next = 0;
  raster_buffer_count = 0;
  clear_vector(position);
  planner_set_position( CONFIG_X_ORIGIN_OFFSET,
                        CONFIG_Y_ORIGIN_OFFSET,
                        CONFIG_Z_ORIGIN_OFFSET );
  position_update_requested = false;
  clear_vector_double(previous_unit_vec);
  previous_nominal_speed = 0.0;
}

int8_t last_raster = 0;

// Process a raster.
// Rasters can be +/- in the x or y directions (not z).
void planner_raster(double x, double y, double z,
                    double feed_rate, double acceleration,
                    uint8_t nominal_laser_intensity,
                    raster_t *raster) {
    double raster_len = 0;
    double head = 0;
    double ramp = pow(feed_rate, 2) / (2 * acceleration);
    uint8_t bidirectional = (raster->bidirectional > 0)?1:0;

    // Calculate how much to offset each raster by to compensate for laser lag
    double offset = (feed_rate * raster->bidirectional / 60.0 / 1000000.0 / 2.0);

    uint8_t *ptr = raster->buffer;
    uint32_t count = raster->length;

    // Truncate the start blank parts.
    for (; *ptr == '0' && count > 0; ptr++, count--, head += raster->dot_size);
    raster->buffer = ptr;
    raster->length = count;

    if (raster->length == 0)
        return;

    // Truncate the end blank parts.
    ptr = raster->buffer + count - 1;
    for (; *ptr == '0' && count > 1; ptr--, count--);
    raster->length = count;

    // Blank line
    if (raster->length == 0)
        return;

    raster_len = raster->dot_size * raster->length;

    x += head;

    // Work out the starting point.
    if (last_raster <= 0)
    {
        // We need to go forwards.
        planner_movement(x - ramp - offset, y, z, feed_rate, acceleration, 0, 0, NULL);
        planner_movement(x - offset, y, z, feed_rate, acceleration, 0, 0, NULL);
    } else {
        // We need to go backwards.
        planner_movement(x + raster_len + ramp + offset, y, z, feed_rate, acceleration, 0, 0, NULL);
        planner_movement(x + raster_len + offset, y, z, feed_rate, acceleration, 0, 0, NULL);
    }

    // Copy the data into our buffer
    // If there isn't space, sit and spin here waiting.
    while (1) {
        if (raster_buffer_count < NUM_RASTERS) {
            raster_buffer_count++;
            if (last_raster <= 0) {
                memcpy(raster_buffer[raster_buffer_next], raster->buffer, raster->length);
            } else {
                uint32_t i;
                uint8_t *dst = raster_buffer[raster_buffer_next] + raster->length;
                uint8_t *src = raster->buffer;
                for (i=0; i<raster->length; ++i)
                {
                    *dst-- = *src++;
                }
            }
            raster->buffer = raster_buffer[raster_buffer_next];
            raster_buffer_next++;
            if (raster_buffer_next == NUM_RASTERS)
                raster_buffer_next = 0;
            break;
        }
    }

    // Etch contiguous dots of the same value.
    raster->intensity = nominal_laser_intensity;

    if (last_raster <= 0)
    {
        // We need to go forwards.
        planner_movement(x + raster_len - offset, y, z, feed_rate, acceleration, 0, 0, raster);
        planner_movement(x + raster_len + ramp - offset, y, z, feed_rate, acceleration, 0, 0, NULL);

        if (bidirectional != 0) {
        	last_raster = 1;
        }
    } else {
        // We need to go backwards.
        planner_movement(x + offset, y, z, feed_rate, acceleration, 0, 0, raster);
        planner_movement(x - ramp + offset, y, z, feed_rate, acceleration, 0, 0, NULL);

        if (bidirectional != 0) {
        	last_raster = -1;
        }
    }
}

// Add a new linear movement to the buffer. x, y and z is
// the signed, absolute target position in millimeters. Feed rate specifies the speed of the motion.
void planner_line(double x, double y, double z,
                  double feed_rate, double acceleration,
                  uint8_t laser_pwm, uint16_t ppi) {
    last_raster = 0;
    planner_movement(x, y, z, feed_rate, acceleration, laser_pwm, ppi, NULL);
}


void planner_dwell(double seconds, uint8_t nominal_laser_intensity) {
// // Execute dwell in seconds. Maximum time delay is > 18 hours, more than enough for any application.
// void mc_dwell(double seconds) {
//    uint16_t i = floor(seconds);
//    stepper_synchronize();
//    _delay_ms(floor(1000*(seconds-i))); // Delay millisecond remainder
//    while (i > 0) {
//      _delay_ms(1000); // Delay one second
//      i--;
//    }
// }  
}


void planner_command(uint8_t type) {
  // calculate the buffer head and check for space
  int next_buffer_head = next_block_index( block_buffer_head ); 
  while(block_buffer_tail == next_buffer_head) {  // buffer full condition
    // good! We are well ahead of the robot. Rest here until buffer has room.
    // sleep_mode();
  }    

  // Prepare to set up new block
  block_t *block = &block_buffer[block_buffer_head];

  // set block type command
  block->block_type = type;

  // Move buffer head
  block_buffer_head = next_buffer_head;

  // make sure the stepper interrupt is processing  
  stepper_wake_up();
}


int planner_blocks_available(void) {
    int next_buffer_head = next_block_index( block_buffer_head );
    if (next_buffer_head == block_buffer_tail)
        return 0;
    else if (next_buffer_head >= block_buffer_tail)
        return BLOCK_BUFFER_SIZE - (next_buffer_head - block_buffer_tail);
    else
        return block_buffer_tail - next_buffer_head;
}

/*
----T****H-----
**H---------T**
*/

block_t *planner_get_current_block()
{
  if (block_buffer_head == block_buffer_tail)
  {
      return(NULL);
  }
  block_buffer_tail_write = next_block_index(block_buffer_tail);
  return(&block_buffer[block_buffer_tail]);
}

void planner_discard_current_block()
{
  if (block_buffer_head != block_buffer_tail)
  {
    if (block_buffer[block_buffer_tail].block_type == BLOCK_TYPE_RASTER_LINE)
    {
        raster_buffer_count--;
    }
    block_buffer_tail = next_block_index( block_buffer_tail );
    block_buffer_tail_write = block_buffer_tail;
  }
}

void planner_reset_block_buffer() {
  block_buffer_head = 0;
  block_buffer_tail = 0;
  block_buffer_tail_write = 0;
  block_buffers_used = 0;

  raster_buffer_count = 0;
  raster_buffer_next = 0;
}




// Reset the planner position vector and planner speed
void planner_set_position(double x, double y, double z) {
  position[X_AXIS] = lround(x*x_steps_per_mm);
  position[Y_AXIS] = lround(y*y_steps_per_mm);
  position[Z_AXIS] = lround(z*CONFIG_Z_STEPS_PER_MM);    
  previous_nominal_speed = 0.0; // resets planner junction speeds
  clear_vector_double(previous_unit_vec);
}

void planner_request_position_update() {
  position_update_requested = true;
}



// Returns the index of the next block in the ring buffer.
static int8_t next_block_index(int8_t block_index) {
  block_index++;
  if (block_index == BLOCK_BUFFER_SIZE) { block_index = 0; }  // cheaper than module (%)
  return block_index;
}

// Returns the index of the previous block in the ring buffer
static int8_t prev_block_index(int8_t block_index) {
  if (block_index == 0) { block_index = BLOCK_BUFFER_SIZE; }
  block_index--;
  return block_index;
}


/*            target rate -> +
**                          /|
**                         / |                 
**                        /  |   
**                       /   |
**      initial rate -> +----+           
**                           ^                   
**                           |                   
**                       DISTANCE 
*/
// Calculates the distance (not time) it takes to accelerate from initial_rate to target_rate
static double estimate_acceleration_distance(double initial_rate, double target_rate, double acceleration) {
  return( (target_rate*target_rate-initial_rate*initial_rate)/(2*acceleration) );
}



/*                        + <- some maximum rate we don't care about
**                       /|\
**                      / | \                    
**                     /  |  + <- final_rate     
**                    /   |  |                   
**   initial_rate -> +----+--+                   
**                        ^  ^                   
**                        |  |                   
**    INTERSECTION_DISTANCE  distance
*/
// This function gives you the point at which you must start braking (at the rate of -acceleration) if 
// you started at speed initial_rate and accelerated until this point and want to end at the final_rate after
// a total travel of distance. This can be used to compute the intersection point between acceleration and
// deceleration in the cases where the trapezoid has no plateau (i.e. never reaches maximum speed)
static double intersection_distance(double initial_rate, double final_rate, double acceleration, double distance) {
  return( (2*acceleration*distance-initial_rate*initial_rate+final_rate*final_rate)/(4*acceleration) );
}

            

/*                      + <- MAX_ALLOWABLE_SPEED
**                      |\
**                      | \                    
**                      |  \    
**                      |   \                  
**                      +----+ <- target velocity            
**                           ^                   
**                           |                   
**                       distance 
*/
// Calculate the beginning speed that results in target_velocity when accelerated over given distance.
static double max_allowable_speed(double acceleration, double target_velocity, double distance) {
  return( sqrt(target_velocity*target_velocity-2*acceleration*distance) );
}



/*                                        
**                                   +--------+   <- nominal_rate
**                                  /|        |\                                
**  nominal_rate*entry_factor ->   + |        | \                               
**                                 | |        |  + <- nominal_rate*exit_factor  
**                                 +-+--------+--+                              
**                                   ^        ^
**                                   |        |
**                      accelerate_until    decelerate_after                           
*/                                                                              
// Calculates accelerate_until and decelerate_after.
static void calculate_trapezoid_for_block(block_t *block, double entry_factor, double exit_factor) {
  block->initial_rate = ceil(block->nominal_rate * entry_factor);  // (step/min)
  block->final_rate = ceil(block->nominal_rate * exit_factor);     // (step/min)
  int32_t acceleration_per_minute = block->rate_delta * ACCELERATION_TICKS_PER_SECOND * 60; // (step/min^2)
  int32_t accelerate_steps = 
    ceil(estimate_acceleration_distance(block->initial_rate, block->nominal_rate, acceleration_per_minute));
  int32_t decelerate_steps = 
    floor(estimate_acceleration_distance(block->nominal_rate, block->final_rate, -acceleration_per_minute));
    
  // Calculate the size of Plateau of Nominal Rate. 
  int32_t plateau_steps = block->step_event_count-accelerate_steps-decelerate_steps;
  
  // Handle special case where we don't reach a plateau.
  if (plateau_steps < 0) {  
    accelerate_steps = ceil( intersection_distance( block->initial_rate, block->final_rate, 
                             acceleration_per_minute, block->step_event_count ) );
    accelerate_steps = max(accelerate_steps, 0);  // check limits due to numerical round-off
    accelerate_steps = min(accelerate_steps, block->step_event_count);
    plateau_steps = 0;
  }  
  
  block->accelerate_until = accelerate_steps;
  block->decelerate_after = accelerate_steps+plateau_steps;
}


static void reduce_entry_speed_reverse(block_t *current, block_t *next) {
  // 'next' here is the newer/later block, not the next in the iteration
  //                   time->
  //     [tail][][][current][next][][][][head] -> loops around to tail
  //     processing ->                  queuing->
  //  
  // Reduce entry_speed if necessary so next entry_speed can definitely be reached with
  // fixed acceleration. This is specifically relevant for short blocks that never plateau.
  // Skip if we already flagged the block as plateauing or vmax <= next entry_speed. 
  if ((!current->nominal_length_flag) && (current->vmax_junction > next->entry_speed)) {
    current->entry_speed = min( current->vmax_junction, max_allowable_speed(
                  -next->acceleration, next->entry_speed, current->millimeters) );
  } else {
    current->entry_speed = current->vmax_junction;
  } 
  current->recalculate_flag = true;
  // no worries about last block, forward pass takes care of it
}


static void reduce_entry_speed_forward(block_t *previous, block_t *current) {
  // 'previous' here is the older/earlier block, not the previous in the iteration
  //                   time->
  //     [tail][][][previous][current][][][][head] -> loops around to tail
  //     processing ->                  queuing->
  //  
  // Reduce entry_speed if necessary so it can be reached from previous entry_speed  with
  // fixed acceleration. This is specifically relevant for short blocks that never plateau.
  // Skip if we already flagged the previous block as plateauing or entry_speed <= previous entry_speed.   
  if (!previous->nominal_length_flag) {
    if (previous->entry_speed < current->entry_speed) {
      double entry_speed = min( current->entry_speed,
        max_allowable_speed(-current->acceleration, previous->entry_speed, previous->millimeters) );
      // Check for junction speed change
      if (current->entry_speed != entry_speed) {
        current->entry_speed = entry_speed;
        current->recalculate_flag = true;
      }
    }    
  }
}


// planner, called whenever a new block was added
// All planner computations are performed with doubles (float on Arduinos) to minimize numerical round-
// off errors. Only when planned values are converted to stepper rate parameters, these are integers.
static void planner_recalculate() {
  //// reverse pass
  // Recalculate entry_speed to be (a) less or equal to vmax_junction and
  // (b) low enough so it can definitely reach the next entry_speed at fixed acceleration.
  int8_t block_index = block_buffer_head;
  block_t *previous = NULL;  // block closer to tail (older)
  block_t *current = NULL;   // block who's entry_speed to be adjusted
  block_t *next = NULL;      // block closer to head (newer)
  while(block_index != block_buffer_tail_write) {
    block_index = prev_block_index( block_index );
    next = current;
    current = previous;
    previous = &block_buffer[block_index];
    if (current && next) {
      reduce_entry_speed_reverse(current, next);
    }
  } // skip tail/first block
  
  //// forward pass
  // Recalculate entry_speed to be low enough it can definitely 
  // be reached from previous entry_speed at fixed acceleration.
  block_index = block_buffer_tail_write;
  previous = NULL;  // block closer to tail (older)
  current = NULL;   // block who's entry_speed to be adjusted
  next = NULL;      // block closer to head (newer)
  while(block_index != block_buffer_head) {
    previous = current;
    current = next;
    next = &block_buffer[block_index];
    if (previous && current) {
      reduce_entry_speed_forward(previous, current);
    }
    block_index = next_block_index(block_index);
  }
  if (current && next) {
    reduce_entry_speed_forward(current, next);
  }
  
  //// recalculate trapeziods for all flagged blocks
  // At this point all blocks have entry_speeds that that can be (a) reached from the prevous
  // entry_speed with the one and only acceleration from our settings and (b) have junction
  // speeds that do not exceed our limits for given direction change.
  // Now we only need to calculate the actual accelerate_until and decelerate_after values.
  block_index = block_buffer_tail_write;
  current = NULL;
  next = NULL;
  while(block_index != block_buffer_head) {
    current = next;
    next = &block_buffer[block_index];
    if (current) {
      if (current->recalculate_flag || next->recalculate_flag) {
        calculate_trapezoid_for_block( current, 
            current->entry_speed/current->nominal_speed, 
            next->entry_speed/current->nominal_speed );      
        current->recalculate_flag = false;
      }
    }
    block_index = next_block_index( block_index );
  }
  // always recalculate last (newest) block with zero exit speed
  calculate_trapezoid_for_block( next, 
    next->entry_speed/next->nominal_speed, ZERO_SPEED/next->nominal_speed );
  next->recalculate_flag = false;
}

