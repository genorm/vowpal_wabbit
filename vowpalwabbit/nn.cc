/*
Copyright (c) by respective owners including Yahoo!, Microsoft, and
individual contributors. All rights reserved.  Released under a BSD (revised)
license as described in the file LICENSE.
 */
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <sstream>

#include "reductions.h"
#include "constant.h"
#include "simple_label.h"
#include "rand48.h"
#include "gd.h"

using namespace std;
using namespace LEARNER;

namespace NN {
  const float hidden_min_activation = -3;
  const float hidden_max_activation = 3;
  const uint32_t nn_constant = 533357803;
  
  struct nn {
    uint32_t k;
    loss_function* squared_loss;
    example output_layer;
    float prediction;
    size_t increment;
    bool dropout;
    uint64_t xsubi;
    uint64_t save_xsubi;
    bool inpass;
    bool finished_setup;

    vw* all;
  };

#define cast_uint32_t static_cast<uint32_t>

  static inline float
  fastpow2 (float p)
  {
    float offset = (p < 0) ? 1.0f : 0.0f;
    float clipp = (p < -126) ? -126.0f : p;
    int w = (int)clipp;
    float z = clipp - w + offset;
    union { uint32_t i; float f; } v = { cast_uint32_t ( (1 << 23) * (clipp + 121.2740575f + 27.7280233f / (4.84252568f - z) - 1.49012907f * z) ) };

    return v.f;
  }

  static inline float
  fastexp (float p)
  {
    return fastpow2 (1.442695040f * p);
  }

  static inline float
  fasttanh (float p)
  {
    return -1.0f + 2.0f / (1.0f + fastexp (-2.0f * p));
  }

  void finish_setup (nn& n, vw& all)
  {
    // TODO: output_layer audit

    memset (&n.output_layer, 0, sizeof (n.output_layer));
    n.output_layer.indices.push_back(nn_output_namespace);
    feature output = {1., nn_constant << all.reg.stride_shift};

    for (unsigned int i = 0; i < n.k; ++i)
      {
        n.output_layer.atomics[nn_output_namespace].push_back(output);
        ++n.output_layer.num_features;
        output.weight_index += (uint32_t)n.increment;
      }

    if (! n.inpass) 
      {
        n.output_layer.atomics[nn_output_namespace].push_back(output);
        ++n.output_layer.num_features;
      }

    n.output_layer.in_use = true;

    n.finished_setup = true;
  }

  void end_pass(nn& n)
  {
    if (n.all->bfgs)
      n.xsubi = n.save_xsubi;
  }

  template <bool is_learn>
  void predict_or_learn(nn& n, base_learner& base, example& ec)
  {
    bool shouldOutput = n.all->raw_prediction > 0;

    if (! n.finished_setup)
      finish_setup (n, *(n.all));

    shared_data sd;
    memcpy (&sd, n.all->sd, sizeof(shared_data));
    shared_data* save_sd = n.all->sd;
    n.all->sd = &sd;

    label_data ld = ec.l.simple;
    void (*save_set_minmax) (shared_data*, float) = n.all->set_minmax;
    float save_min_label;
    float save_max_label;
    float dropscale = n.dropout ? 2.0f : 1.0f;
    loss_function* save_loss = n.all->loss;

    float* hidden_units = (float*) alloca (n.k * sizeof (float));
    bool* dropped_out = (bool*) alloca (n.k * sizeof (bool));
  
    string outputString;
    stringstream outputStringStream(outputString);

    n.all->set_minmax = noop_mm;
    n.all->loss = n.squared_loss;
    save_min_label = n.all->sd->min_label;
    n.all->sd->min_label = hidden_min_activation;
    save_max_label = n.all->sd->max_label;
    n.all->sd->max_label = hidden_max_activation;
    for (unsigned int i = 0; i < n.k; ++i)
      {
        uint32_t biasindex = (uint32_t) constant * (n.all->wpp << n.all->reg.stride_shift) + i * (uint32_t)n.increment + ec.ft_offset;
        weight* w = &n.all->reg.weight_vector[biasindex & n.all->reg.weight_mask];
        
        // avoid saddle point at 0
        if (*w == 0)
          {
            w[0] = (float) (frand48 () - 0.5);

            if (n.dropout && n.all->normalized_updates)
              w[n.all->normalized_idx] = 1e-4f;
          }

	base.predict(ec, i);

        hidden_units[i] = ec.pred.scalar;

        dropped_out[i] = (n.dropout && merand48 (n.xsubi) < 0.5);

        if (shouldOutput) {
          if (i > 0) outputStringStream << ' ';
          outputStringStream << i << ':' << ec.partial_prediction << ',' << fasttanh (hidden_units[i]);
        }
      }
    n.all->loss = save_loss;
    n.all->set_minmax = save_set_minmax;
    n.all->sd->min_label = save_min_label;
    n.all->sd->max_label = save_max_label;

    bool converse = false;
    float save_partial_prediction = 0;
    float save_final_prediction = 0;
    float save_ec_loss = 0;

CONVERSE: // That's right, I'm using goto.  So sue me.

    n.output_layer.total_sum_feat_sq = 1;
    n.output_layer.sum_feat_sq[nn_output_namespace] = 1;

    for (unsigned int i = 0; i < n.k; ++i)
      {
        float sigmah = 
          (dropped_out[i]) ? 0.0f : dropscale * fasttanh (hidden_units[i]);
        n.output_layer.atomics[nn_output_namespace][i].x = sigmah;

        n.output_layer.total_sum_feat_sq += sigmah * sigmah;
        n.output_layer.sum_feat_sq[nn_output_namespace] += sigmah * sigmah;

        uint32_t nuindex = n.output_layer.atomics[nn_output_namespace][i].weight_index + (n.k * (uint32_t)n.increment) + ec.ft_offset;
        weight* w = &n.all->reg.weight_vector[nuindex & n.all->reg.weight_mask];
        
        // avoid saddle point at 0
        if (*w == 0)
          {
            float sqrtk = sqrt ((float)n.k);
            w[0] = (float) (frand48 () - 0.5) / sqrtk;

            if (n.dropout && n.all->normalized_updates)
              w[n.all->normalized_idx] = 1e-4f;
          }
      }

    if (n.inpass) {
      // TODO: this is not correct if there is something in the 
      // nn_output_namespace but at least it will not leak memory
      // in that case

      ec.indices.push_back (nn_output_namespace);
      v_array<feature> save_nn_output_namespace = ec.atomics[nn_output_namespace];
      ec.atomics[nn_output_namespace] = n.output_layer.atomics[nn_output_namespace];
      ec.sum_feat_sq[nn_output_namespace] = n.output_layer.sum_feat_sq[nn_output_namespace];
      ec.total_sum_feat_sq += n.output_layer.sum_feat_sq[nn_output_namespace];
      if (is_learn)
	base.learn(ec, n.k);
      else
	base.predict(ec, n.k);
      n.output_layer.partial_prediction = ec.partial_prediction;
      n.output_layer.loss = ec.loss;
      ec.total_sum_feat_sq -= n.output_layer.sum_feat_sq[nn_output_namespace];
      ec.sum_feat_sq[nn_output_namespace] = 0;
      ec.atomics[nn_output_namespace] = save_nn_output_namespace;
      ec.indices.pop ();
    }
    else {
      n.output_layer.ft_offset = ec.ft_offset;
      n.output_layer.l = ec.l;
      n.output_layer.partial_prediction = 0;
      n.output_layer.example_t = ec.example_t;
      if (is_learn)
	base.learn(n.output_layer, n.k);
      else
	base.predict(n.output_layer, n.k);
      ec.l = n.output_layer.l;
    }

    n.prediction = GD::finalize_prediction (n.all->sd, n.output_layer.partial_prediction);

    if (shouldOutput) {
      outputStringStream << ' ' << n.output_layer.partial_prediction;
      n.all->print_text(n.all->raw_prediction, outputStringStream.str(), ec.tag);
    }
    
    if (is_learn && n.all->training && ld.label != FLT_MAX) {
      float gradient = n.all->loss->first_derivative(n.all->sd, 
						     n.prediction,
						     ld.label);

      if (fabs (gradient) > 0) {
        n.all->loss = n.squared_loss;
        n.all->set_minmax = noop_mm;
        save_min_label = n.all->sd->min_label;
        n.all->sd->min_label = hidden_min_activation;
        save_max_label = n.all->sd->max_label;
        n.all->sd->max_label = hidden_max_activation;

        for (unsigned int i = 0; i < n.k; ++i) {
          if (! dropped_out[i]) {
            float sigmah = 
              n.output_layer.atomics[nn_output_namespace][i].x / dropscale;
            float sigmahprime = dropscale * (1.0f - sigmah * sigmah);
            uint32_t nuindex = n.output_layer.atomics[nn_output_namespace][i].weight_index + (n.k * (uint32_t)n.increment) + ec.ft_offset;
            float nu = n.all->reg.weight_vector[nuindex & n.all->reg.weight_mask];
            float gradhw = 0.5f * nu * gradient * sigmahprime;

            ec.l.simple.label = GD::finalize_prediction (n.all->sd, hidden_units[i] - gradhw);
            if (ec.l.simple.label != hidden_units[i]) 
              base.learn(ec, i);
          }
        }

        n.all->loss = save_loss;
        n.all->set_minmax = save_set_minmax;
        n.all->sd->min_label = save_min_label;
        n.all->sd->max_label = save_max_label;
      }
    }

    ec.l.simple.label = ld.label;

    if (! converse) {
      save_partial_prediction = n.output_layer.partial_prediction;
      save_final_prediction = n.prediction;
      save_ec_loss = n.output_layer.loss;
    }

    if (n.dropout && ! converse)
      {
        for (unsigned int i = 0; i < n.k; ++i)
          {
            dropped_out[i] = ! dropped_out[i];
          }

        converse = true;
        goto CONVERSE;
      }

    ec.partial_prediction = save_partial_prediction;
    ec.pred.scalar = save_final_prediction;
    ec.loss = save_ec_loss;

    n.all->sd = save_sd;
    n.all->set_minmax (n.all->sd, sd.min_label);
    n.all->set_minmax (n.all->sd, sd.max_label);
  }

  void finish_example(vw& all, nn&, example& ec)
  {
    int save_raw_prediction = all.raw_prediction;
    all.raw_prediction = -1;
    return_simple_example(all, NULL, ec);
    all.raw_prediction = save_raw_prediction;
  }

  void finish(nn& n)
  {
    delete n.squared_loss;
    free (n.output_layer.indices.begin);
    free (n.output_layer.atomics[nn_output_namespace].begin);
  }

  base_learner* setup(vw& all)
  {
    new_options(all, "Neural Network options")
      ("nn", po::value<size_t>(), "Use sigmoidal feedforward network with <k> hidden units");
    if(missing_required(all)) return NULL;
    new_options(all)
      ("inpass", "Train or test sigmoidal feedforward network with input passthrough.")
      ("dropout", "Train or test sigmoidal feedforward network using dropout.")
      ("meanfield", "Train or test sigmoidal feedforward network using mean field.");
    add_options(all);

    po::variables_map& vm = all.vm;
    nn& n = calloc_or_die<nn>();
    n.all = &all;
    //first parse for number of hidden units
    n.k = (uint32_t)vm["nn"].as<size_t>();
    *all.file_options << " --nn " << n.k;

    if ( vm.count("dropout") ) {
      n.dropout = true;
      *all.file_options << " --dropout ";
    }
    
    if ( vm.count("meanfield") ) {
      n.dropout = false;
      if (! all.quiet) 
        std::cerr << "using mean field for neural network " 
                  << (all.training ? "training" : "testing") 
                  << std::endl;
    }

    if (n.dropout) 
      if (! all.quiet)
        std::cerr << "using dropout for neural network "
                  << (all.training ? "training" : "testing") 
                  << std::endl;

    if (vm.count ("inpass")) {
      n.inpass = true;
      *all.file_options << " --inpass";

    }

    if (n.inpass && ! all.quiet)
      std::cerr << "using input passthrough for neural network "
                << (all.training ? "training" : "testing") 
                << std::endl;

    n.finished_setup = false;
    n.squared_loss = getLossFunction (all, "squared", 0);

    n.xsubi = 0;

    if (vm.count("random_seed"))
      n.xsubi = vm["random_seed"].as<size_t>();

    n.save_xsubi = n.xsubi;
    
    base_learner* base = setup_base(all);
    n.increment = base->increment;//Indexing of output layer is odd.
    learner<nn>& l = init_learner(&n, base, predict_or_learn<true>, 
				  predict_or_learn<false>, n.k+1);
    l.set_finish(finish);
    l.set_finish_example(finish_example);
    l.set_end_pass(end_pass);

    return make_base(l);
  }
}
