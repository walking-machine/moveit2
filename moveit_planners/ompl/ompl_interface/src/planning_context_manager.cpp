/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2012, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Ioan Sucan */

#include <moveit/ompl_interface/planning_context_manager.h>
#include <moveit/robot_state/conversions.h>

#include <utility>

#include <ompl/geometric/planners/AnytimePathShortening.h>
#include <ompl/geometric/planners/rrt/RRT.h>
#include <ompl/geometric/planners/rrt/pRRT.h>
#include <ompl/geometric/planners/rrt/RRTConnect.h>
#include <ompl/geometric/planners/rrt/TRRT.h>
#include <ompl/geometric/planners/rrt/LazyRRT.h>
#include <ompl/geometric/planners/est/EST.h>
#include <ompl/geometric/planners/sbl/SBL.h>
#include <ompl/geometric/planners/sbl/pSBL.h>
#include <ompl/geometric/planners/kpiece/KPIECE1.h>
#include <ompl/geometric/planners/kpiece/BKPIECE1.h>
#include <ompl/geometric/planners/kpiece/LBKPIECE1.h>
#include <ompl/geometric/planners/rrt/RRTstar.h>
#include <ompl/geometric/planners/prm/PRM.h>
#include <ompl/geometric/planners/prm/PRMstar.h>
#include <ompl/geometric/planners/fmt/FMT.h>
#include <ompl/geometric/planners/fmt/BFMT.h>
#include <ompl/geometric/planners/pdst/PDST.h>
#include <ompl/geometric/planners/stride/STRIDE.h>
#include <ompl/geometric/planners/rrt/BiTRRT.h>
#include <ompl/geometric/planners/rrt/LBTRRT.h>
#include <ompl/geometric/planners/est/BiEST.h>
#include <ompl/geometric/planners/est/ProjEST.h>
#include <ompl/geometric/planners/prm/LazyPRM.h>
#include <ompl/geometric/planners/prm/LazyPRMstar.h>
#include <ompl/geometric/planners/prm/SPARS.h>
#include <ompl/geometric/planners/prm/SPARStwo.h>
#include <ompl/geometric/planners/prm/PRMcustom.h>

#include <ompl/base/ConstrainedSpaceInformation.h>
#include <ompl/base/spaces/constraint/ProjectedStateSpace.h>

#include <moveit/ompl_interface/parameterization/joint_space/joint_model_state_space_factory.h>
#include <moveit/ompl_interface/parameterization/joint_space/joint_model_state_space.h>
#include <moveit/ompl_interface/parameterization/joint_space/constrained_planning_state_space_factory.h>
#include <moveit/ompl_interface/parameterization/joint_space/constrained_planning_state_space.h>
#include <moveit/ompl_interface/parameterization/work_space/pose_model_state_space_factory.h>
#include <moveit/ompl_interface/detail/ompl_constraints.h>

using namespace std::placeholders;

namespace ompl_interface
{
static const rclcpp::Logger LOGGER = rclcpp::get_logger("moveit.ompl_planning.planning_context_manager");

struct PlanningContextManager::CachedContexts
{
  std::map<std::pair<std::string, std::string>, std::vector<ModelBasedPlanningContextPtr> > contexts_;
  std::mutex lock_;
};

MultiQueryPlannerAllocator::~MultiQueryPlannerAllocator()
{
  // Store all planner data
  for (const auto& entry : planner_data_storage_paths_)
  {
    ob::PlannerData data(planners_[entry.first]->getSpaceInformation());
    planners_[entry.first]->getPlannerData(data);
    RCLCPP_INFO_STREAM(LOGGER, "Storing planner data. NumEdges: " << data.numEdges()
                                                                  << ", NumVertices: " << data.numVertices());
    storage_.store(data, entry.second.c_str());
  }
}

template <typename T>
ompl::base::PlannerPtr MultiQueryPlannerAllocator::allocatePlanner(const ob::SpaceInformationPtr& si,
                                                                   const std::string& new_name,
                                                                   const ModelBasedPlanningContextSpecification& spec)
{
  // Store planner instance if multi-query planning is enabled
  auto cfg = spec.config_;
  auto it = cfg.find("multi_query_planning_enabled");
  bool multi_query_planning_enabled = false;
  if (it != cfg.end())
  {
    multi_query_planning_enabled = boost::lexical_cast<bool>(it->second);
    cfg.erase(it);
  }
  if (multi_query_planning_enabled)
  {
    // If we already have an instance, reuse it's planning data
    // FIXME: make reusing PlannerPtr not crash, so that we don't have to reconstruct a PlannerPtr instance
    auto planner_map_it = planners_.find(new_name);
    if (planner_map_it != planners_.end())
    {
      ob::PlannerData data(si);
      planner_map_it->second->getPlannerData(data);
      RCLCPP_INFO_STREAM(LOGGER, "Reusing planner data. NumEdges: " << data.numEdges()
                                                                    << ", NumVertices: " << data.numVertices());
      planners_[planner_map_it->first] = std::shared_ptr<ob::Planner>{ allocatePersistentPlanner<T>(data) };
      return planners_[planner_map_it->first];
    }

    // Certain multi-query planners allow loading and storing the generated planner data. This feature can be
    // selectively enabled for loading and storing using the bool parameters 'load_planner_data' and
    // 'store_planner_data'. The storage file path is set using the parameter 'planner_data_path'.
    // File read and write access are handled by the PlannerDataStorage class. If the file path is invalid
    // an error message is printed and the planner is constructed/destructed with default values.
    it = cfg.find("load_planner_data");
    bool load_planner_data = false;
    if (it != cfg.end())
    {
      load_planner_data = boost::lexical_cast<bool>(it->second);
      cfg.erase(it);
    }
    it = cfg.find("store_planner_data");
    bool store_planner_data = false;
    if (it != cfg.end())
    {
      store_planner_data = boost::lexical_cast<bool>(it->second);
      cfg.erase(it);
    }
    it = cfg.find("planner_data_path");
    std::string planner_data_path;
    if (it != cfg.end())
    {
      planner_data_path = it->second;
      cfg.erase(it);
    }
    // Store planner instance for multi-query use
    planners_[new_name] =
        allocatePlannerImpl<T>(si, new_name, spec, load_planner_data, store_planner_data, planner_data_path);
    return planners_[new_name];
  }
  else
  {
    // Return single-shot planner instance
    return allocatePlannerImpl<T>(si, new_name, spec);
  }
}

template <typename T>
ompl::base::PlannerPtr
MultiQueryPlannerAllocator::allocatePlannerImpl(const ob::SpaceInformationPtr& si, const std::string& new_name,
                                                const ModelBasedPlanningContextSpecification& spec,
                                                bool load_planner_data, bool store_planner_data,
                                                const std::string& file_path)
{
  ob::PlannerPtr planner;
  // Try to initialize planner with loaded planner data
  if (load_planner_data)
  {
    ob::PlannerData data(si);
    storage_.load(file_path.c_str(), data);
    RCLCPP_INFO_STREAM(LOGGER, "Loading planner data. NumEdges: " << data.numEdges()
                                                                  << ", NumVertices: " << data.numVertices());
    planner = std::shared_ptr<ob::Planner>{ allocatePersistentPlanner<T>(data) };
    if (!planner)
    {
      RCLCPP_ERROR(LOGGER,
                   "Creating a '%s' planner from persistent data is not supported. Going to create a new instance.",
                   new_name.c_str());
    }
  }

  if (!planner)
  {
    planner = std::make_shared<T>(si);
  }

  if (!new_name.empty())
  {
    planner->setName(new_name);
  }

  planner->params().setParams(spec.config_, true);
  //  Remember which planner instances to store when the destructor is called
  if (store_planner_data)
  {
    planner_data_storage_paths_[new_name] = file_path;
  }

  return planner;
}

// default implementation
template <typename T>
inline ompl::base::Planner* MultiQueryPlannerAllocator::allocatePersistentPlanner(const ob::PlannerData& /*data*/)
{
  return nullptr;
};
template <>
inline ompl::base::Planner*
MultiQueryPlannerAllocator::allocatePersistentPlanner<ompl::geometric::PRM>(const ob::PlannerData& data)
{
  return new og::PRM(data);
};
template <>
inline ompl::base::Planner*
MultiQueryPlannerAllocator::allocatePersistentPlanner<ompl::geometric::PRMcustom>(const ob::PlannerData& data)
{
  return new og::PRMcustom(data);
};
template <>
inline ompl::base::Planner*
MultiQueryPlannerAllocator::allocatePersistentPlanner<ompl::geometric::PRMstar>(const ob::PlannerData& data)
{
  return new og::PRMstar(data);
};
template <>
inline ompl::base::Planner*
MultiQueryPlannerAllocator::allocatePersistentPlanner<ompl::geometric::LazyPRM>(const ob::PlannerData& data)
{
  return new og::LazyPRM(data);
};
template <>
inline ompl::base::Planner*
MultiQueryPlannerAllocator::allocatePersistentPlanner<ompl::geometric::LazyPRMstar>(const ob::PlannerData& data)
{
  return new og::LazyPRMstar(data);
};

PlanningContextManager::PlanningContextManager(moveit::core::RobotModelConstPtr robot_model,
                                               constraint_samplers::ConstraintSamplerManagerPtr csm)
  : robot_model_(std::move(robot_model))
  , constraint_sampler_manager_(std::move(csm))
  , max_goal_samples_(10)
  , max_state_sampling_attempts_(4)
  , max_goal_sampling_attempts_(1000)
  , max_planning_threads_(4)
  , max_solution_segment_length_(0.0)
  , minimum_waypoint_count_(2)
{
  cached_contexts_ = std::make_shared<CachedContexts>();
  registerDefaultPlanners();
  registerDefaultStateSpaces();
}

PlanningContextManager::~PlanningContextManager() = default;

ConfiguredPlannerAllocator PlanningContextManager::plannerSelector(const std::string& planner) const
{
  auto it = known_planners_.find(planner);
  if (it != known_planners_.end())
  {
    return it->second;
  }
  else
  {
    RCLCPP_ERROR(LOGGER, "Unknown planner: '%s'", planner.c_str());
    return ConfiguredPlannerAllocator();
  }
}

template <typename T>
void PlanningContextManager::registerPlannerAllocatorHelper(const std::string& planner_id)
{
  registerPlannerAllocator(planner_id, [&](const ob::SpaceInformationPtr& si, const std::string& new_name,
                                           const ModelBasedPlanningContextSpecification& spec) {
    return planner_allocator_.allocatePlanner<T>(si, new_name, spec);
  });
}

void PlanningContextManager::registerDefaultPlanners()
{
  registerPlannerAllocatorHelper<og::AnytimePathShortening>("geometric::AnytimePathShortening");
  registerPlannerAllocatorHelper<og::BFMT>("geometric::BFMT");
  registerPlannerAllocatorHelper<og::BiEST>("geometric::BiEST");
  registerPlannerAllocatorHelper<og::BiTRRT>("geometric::BiTRRT");
  registerPlannerAllocatorHelper<og::BKPIECE1>("geometric::BKPIECE");
  registerPlannerAllocatorHelper<og::EST>("geometric::EST");
  registerPlannerAllocatorHelper<og::FMT>("geometric::FMT");
  registerPlannerAllocatorHelper<og::KPIECE1>("geometric::KPIECE");
  registerPlannerAllocatorHelper<og::LazyPRM>("geometric::LazyPRM");
  registerPlannerAllocatorHelper<og::LazyPRMstar>("geometric::LazyPRMstar");
  registerPlannerAllocatorHelper<og::LazyRRT>("geometric::LazyRRT");
  registerPlannerAllocatorHelper<og::LBKPIECE1>("geometric::LBKPIECE");
  registerPlannerAllocatorHelper<og::LBTRRT>("geometric::LBTRRT");
  registerPlannerAllocatorHelper<og::PDST>("geometric::PDST");
  registerPlannerAllocatorHelper<og::PRM>("geometric::PRM");
  registerPlannerAllocatorHelper<og::PRMstar>("geometric::PRMstar");
  registerPlannerAllocatorHelper<og::ProjEST>("geometric::ProjEST");
  registerPlannerAllocatorHelper<og::RRT>("geometric::RRT");
  registerPlannerAllocatorHelper<og::RRTConnect>("geometric::RRTConnect");
  registerPlannerAllocatorHelper<og::RRTstar>("geometric::RRTstar");
  registerPlannerAllocatorHelper<og::SBL>("geometric::SBL");
  registerPlannerAllocatorHelper<og::SPARS>("geometric::SPARS");
  registerPlannerAllocatorHelper<og::SPARStwo>("geometric::SPARStwo");
  registerPlannerAllocatorHelper<og::STRIDE>("geometric::STRIDE");
  registerPlannerAllocatorHelper<og::TRRT>("geometric::TRRT");
  registerPlannerAllocatorHelper<og::PRMcustom>("geometric::PRMcustom");
}

void PlanningContextManager::registerDefaultStateSpaces()
{
  registerStateSpaceFactory(std::make_shared<JointModelStateSpaceFactory>());
  registerStateSpaceFactory(std::make_shared<PoseModelStateSpaceFactory>());
  registerStateSpaceFactory(std::make_shared<ConstrainedPlanningStateSpaceFactory>());
}

ConfiguredPlannerSelector PlanningContextManager::getPlannerSelector() const
{
  return [this](const std::string& planner) { return plannerSelector(planner); };
}

void PlanningContextManager::setPlannerConfigurations(const planning_interface::PlannerConfigurationMap& pconfig)
{
  planner_configs_ = pconfig;
}

ModelBasedPlanningContextPtr
PlanningContextManager::getPlanningContext(const planning_interface::PlannerConfigurationSettings& config,
                                           const ModelBasedStateSpaceFactoryPtr& factory,
                                           const moveit_msgs::msg::MotionPlanRequest& req) const
{
  // Check for a cached planning context
  ModelBasedPlanningContextPtr context;

  {
    std::unique_lock<std::mutex> slock(cached_contexts_->lock_);
    auto cached_contexts = cached_contexts_->contexts_.find(std::make_pair(config.name, factory->getType()));
    if (cached_contexts != cached_contexts_->contexts_.end())
    {
      for (const ModelBasedPlanningContextPtr& cached_context : cached_contexts->second)
        if (cached_context.unique())
        {
          RCLCPP_DEBUG(LOGGER, "Reusing cached planning context");
          context = cached_context;
          break;
        }
    }
  }

  // Create a new planning context
  if (!context)
  {
    ModelBasedStateSpaceSpecification space_spec(robot_model_, config.group);
    ModelBasedPlanningContextSpecification context_spec;
    context_spec.config_ = config.config;
    context_spec.planner_selector_ = getPlannerSelector();
    context_spec.constraint_sampler_manager_ = constraint_sampler_manager_;
    context_spec.state_space_ = factory->getNewStateSpace(space_spec);

    if (factory->getType() == ConstrainedPlanningStateSpace::PARAMETERIZATION_TYPE)
    {
      RCLCPP_DEBUG_STREAM(LOGGER, "planning_context_manager: Using OMPL's constrained state space for planning.");

      // Select the correct type of constraints based on the path constraints in the planning request.
      ompl::base::ConstraintPtr ompl_constraint =
          createOMPLConstraints(robot_model_, config.group, req.path_constraints);

      // Create a constrained state space of type "projected state space".
      // Other types are available, so we probably should add another setting to ompl_planning.yaml
      // to choose between them.
      context_spec.constrained_state_space_ =
          std::make_shared<ob::ProjectedStateSpace>(context_spec.state_space_, ompl_constraint);

      // Pass the constrained state space to ompl simple setup through the creation of a
      // ConstrainedSpaceInformation object. This makes sure the state space is properly initialized.
      context_spec.ompl_simple_setup_ = std::make_shared<ompl::geometric::SimpleSetup>(
          std::make_shared<ob::ConstrainedSpaceInformation>(context_spec.constrained_state_space_));
    }
    else
    {
      // Choose the correct simple setup type to load
      context_spec.ompl_simple_setup_ = std::make_shared<ompl::geometric::SimpleSetup>(context_spec.state_space_);
    }

    RCLCPP_DEBUG(LOGGER, "Creating new planning context");
    context = std::make_shared<ModelBasedPlanningContext>(config.name, context_spec);

    // Do not cache a constrained planning context, as the constraints could be changed
    // and need to be parsed again.
    if (factory->getType() != ConstrainedPlanningStateSpace::PARAMETERIZATION_TYPE)
    {
      {
        std::lock_guard<std::mutex> slock(cached_contexts_->lock_);
        cached_contexts_->contexts_[std::make_pair(config.name, factory->getType())].push_back(context);
      }
    }
  }

  context->setMaximumPlanningThreads(max_planning_threads_);
  context->setMaximumGoalSamples(max_goal_samples_);
  context->setMaximumStateSamplingAttempts(max_state_sampling_attempts_);
  context->setMaximumGoalSamplingAttempts(max_goal_sampling_attempts_);

  if (max_solution_segment_length_ > std::numeric_limits<double>::epsilon())
  {
    context->setMaximumSolutionSegmentLength(max_solution_segment_length_);
  }

  context->setMinimumWaypointCount(minimum_waypoint_count_);
  context->setSpecificationConfig(config.config);

  return context;
}

const ModelBasedStateSpaceFactoryPtr& PlanningContextManager::getStateSpaceFactory(const std::string& factory_type) const
{
  auto f = factory_type.empty() ? state_space_factories_.begin() : state_space_factories_.find(factory_type);
  if (f != state_space_factories_.end())
  {
    RCLCPP_DEBUG(LOGGER, "Using '%s' parameterization for solving problem", factory_type.c_str());
    return f->second;
  }
  else
  {
    RCLCPP_ERROR(LOGGER, "Factory of type '%s' was not found", factory_type.c_str());
    static const ModelBasedStateSpaceFactoryPtr EMPTY;
    return EMPTY;
  }
}

const ModelBasedStateSpaceFactoryPtr&
PlanningContextManager::getStateSpaceFactory(const std::string& group,
                                             const moveit_msgs::msg::MotionPlanRequest& req) const
{
  // find the problem representation to use
  auto best = state_space_factories_.end();
  int prev_priority = 0;
  for (auto it = state_space_factories_.begin(); it != state_space_factories_.end(); ++it)
  {
    int priority = it->second->canRepresentProblem(group, req, robot_model_);
    if (priority > prev_priority)
    {
      best = it;
      prev_priority = priority;
    }
  }

  if (best == state_space_factories_.end())
  {
    RCLCPP_ERROR(LOGGER, "There are no known state spaces that can represent the given planning "
                         "problem");
    static const ModelBasedStateSpaceFactoryPtr EMPTY;
    return EMPTY;
  }
  else
  {
    RCLCPP_DEBUG(LOGGER, "Using '%s' parameterization for solving problem", best->first.c_str());
    return best->second;
  }
}

ModelBasedPlanningContextPtr PlanningContextManager::getPlanningContext(
    const planning_scene::PlanningSceneConstPtr& planning_scene, const moveit_msgs::msg::MotionPlanRequest& req,
    moveit_msgs::msg::MoveItErrorCodes& error_code, const rclcpp::Node::SharedPtr& node,
    bool use_constraints_approximation) const
{
  if (req.group_name.empty())
  {
    RCLCPP_ERROR(LOGGER, "No group specified to plan for");
    error_code.val = moveit_msgs::msg::MoveItErrorCodes::INVALID_GROUP_NAME;
    return ModelBasedPlanningContextPtr();
  }

  error_code.val = moveit_msgs::msg::MoveItErrorCodes::FAILURE;

  if (!planning_scene)
  {
    RCLCPP_ERROR(LOGGER, "No planning scene supplied as input");
    return ModelBasedPlanningContextPtr();
  }

  // identify the correct planning configuration
  auto pc = planner_configs_.end();
  if (!req.planner_id.empty())
  {
    pc = planner_configs_.find(req.planner_id.find(req.group_name) == std::string::npos ?
                                   req.group_name + "[" + req.planner_id + "]" :
                                   req.planner_id);
    if (pc == planner_configs_.end())
    {
      RCLCPP_WARN(LOGGER,
                  "Cannot find planning configuration for group '%s' using planner '%s'. Will use defaults instead.",
                  req.group_name.c_str(), req.planner_id.c_str());
    }
  }

  if (pc == planner_configs_.end())
  {
    pc = planner_configs_.find(req.group_name);
    if (pc == planner_configs_.end())
    {
      RCLCPP_ERROR(LOGGER, "Cannot find planning configuration for group '%s'", req.group_name.c_str());
      return ModelBasedPlanningContextPtr();
    }
  }

  // State space selection process
  // ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
  // There are 3 options for the factory_selector
  // 1) enforce_constrained_state_space = true AND there are path constraints in the planning request
  //         Overrides all other settings and selects a ConstrainedPlanningStateSpace factory
  // 2) enforce_joint_model_state_space = true
  //         If 1) is false, then this one overrides the remaining settings and returns a JointModelStateSpace factory
  // 3) Not 1) or 2), then the factory is selected based on the priority that each one returns.
  //         See PoseModelStateSpaceFactory::canRepresentProblem for details on the selection process.
  //         In short, it returns a PoseModelStateSpace if there is an IK solver and a path constraint.
  //
  // enforce_constrained_state_space
  // ****************************************
  // Check if the user wants to use an OMPL ConstrainedStateSpace for planning.
  // This is done by setting 'enforce_constrained_state_space' to 'true' for the desired group in ompl_planing.yaml.
  // If there are no path constraints in the planning request, this option is ignored, as the constrained state space is
  // only useful for paths constraints. (And at the moment only a single position constraint is supported, hence:
  //     req.path_constraints.position_constraints.size() == 1
  // is used in the selection process below.)
  //
  // enforce_joint_model_state_space
  // *******************************
  // Check if sampling in JointModelStateSpace is enforced for this group by user.
  // This is done by setting 'enforce_joint_model_state_space' to 'true' for the desired group in ompl_planning.yaml.
  //
  // Some planning problems like orientation path constraints are represented in PoseModelStateSpace and sampled via IK.
  // However consecutive IK solutions are not checked for proximity at the moment and sometimes happen to be flipped,
  // leading to invalid trajectories. This workaround lets the user prevent this problem by forcing rejection sampling
  // in JointModelStateSpace.
  ModelBasedStateSpaceFactoryPtr factory;
  auto constrained_planning_iterator = pc->second.config.find("enforce_constrained_state_space");
  auto joint_space_planning_iterator = pc->second.config.find("enforce_joint_model_state_space");

  // Use ConstrainedPlanningStateSpace if there is exactly one position constraint and/or one orientation constraint
  if (constrained_planning_iterator != pc->second.config.end() &&
      boost::lexical_cast<bool>(constrained_planning_iterator->second) &&
      ((req.path_constraints.position_constraints.size() == 1) ||
       (req.path_constraints.orientation_constraints.size() == 1)))
  {
    factory = getStateSpaceFactory(ConstrainedPlanningStateSpace::PARAMETERIZATION_TYPE);
  }
  else if (joint_space_planning_iterator != pc->second.config.end() &&
           boost::lexical_cast<bool>(joint_space_planning_iterator->second))
  {
    factory = getStateSpaceFactory(JointModelStateSpace::PARAMETERIZATION_TYPE);
  }
  else
  {
    factory = getStateSpaceFactory(pc->second.group, req);
  }

  ModelBasedPlanningContextPtr context = getPlanningContext(pc->second, factory, req);

  if (context)
  {
    context->clear();

    moveit::core::RobotStatePtr start_state = planning_scene->getCurrentStateUpdated(req.start_state);

    // Setup the context
    context->setPlanningScene(planning_scene);
    context->setMotionPlanRequest(req);
    context->setCompleteInitialState(*start_state);

    context->setPlanningVolume(req.workspace_parameters);
    if (!context->setPathConstraints(req.path_constraints, &error_code))
    {
      return ModelBasedPlanningContextPtr();
    }

    if (!context->setGoalConstraints(req.goal_constraints, req.path_constraints, &error_code))
    {
      return ModelBasedPlanningContextPtr();
    }

    try
    {
      context->configure(node, use_constraints_approximation);
      RCLCPP_DEBUG(LOGGER, "%s: New planning context is set.", context->getName().c_str());
      error_code.val = moveit_msgs::msg::MoveItErrorCodes::SUCCESS;
    }
    catch (ompl::Exception& ex)
    {
      RCLCPP_ERROR(LOGGER, "OMPL encountered an error: %s", ex.what());
      context.reset();
    }
  }

  return context;
}
}  // namespace ompl_interface
