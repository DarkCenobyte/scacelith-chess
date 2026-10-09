#!/usr/bin/env bash
# fit.sh <opt9 args...>: opt9 with the cost weights of the last contact refit, g7 (each overridable
# from the environment; see the top of opt9.cpp). Build it first: ./mk.sh opt9
. "$(dirname "$0")/env.sh"
export W_SKIN=${W_SKIN:-200} W_TSKIN=${W_TSKIN:-20} W_IPT=${W_IPT:-1000} IP_T=${IP_T:-0.9} W_BALL=${W_BALL:-100}
export W_CUFF=${W_CUFF:-50} W_SLIDEV=${W_SLIDEV:-5}
export W_SIDE=${W_SIDE:-1} SIDE_MAX=${SIDE_MAX:-32} W_RATE=${W_RATE:-300} RATE_MAX=${RATE_MAX:-2.1} SLIDE_MARGIN=${SLIDE_MARGIN:-0.0015}
export W_LIFT=${W_LIFT:-30} STRAIN_T=${STRAIN_T:-0.025} STRAIN_H=${STRAIN_H:-0.034}
export W_KNUCKLE=${W_KNUCKLE:-200} W_PALM=${W_PALM:-200} PALM_T=${PALM_T:-0.6} W_CROSS=${W_CROSS:-0.01} CROSS_T=${CROSS_T:-46} PADS_BEHIND=${PADS_BEHIND:-11.5} PADS_WRIST=${PADS_WRIST:-17} PADS_KNUCKLE=${PADS_KNUCKLE:-0.5} W_TIDX=${W_TIDX:-0.3}
export NTHREADS=${NTHREADS:-3}
exec "$HS_WORK/bin/opt9" "$@"
