#!/usr/bin/env sh

GOOGLE_LOG_DIR=models/video_synthesis/log \
    /usr/local/openmpi/bin/mpirun -np 8 \
    build/install/bin/caffe train \
    --solver=models/video_synthesis/refine_label/fcn_ires_v2_solver.prototxt \
    --weights=models/video_synthesis/refine_label/ires_v2_large.caffemodel
	#--weights=models/semantic_segmentation/fcn_ires_v2/snapshot/fcn_ires_v2_coco_iter_80000.caffemodel