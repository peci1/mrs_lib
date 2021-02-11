// clang: MatousFormat
/**  \file
     \brief Defines ParamLoader - a convenience class for loading static ROS parameters.
     \author Matouš Vrba - vrbamato@fel.cvut.cz
 */

#ifndef PARAM_LOADER_H
#define PARAM_LOADER_H

#include <rclcpp/logger.hpp>
#include <rclcpp/parameter_value.hpp>
#include <rclcpp/rclcpp.hpp>
#include <string>
#include <map>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <iostream>

namespace mrs_lib
{
  /* ParamRefWrapper class //{ */


  class ParamRefWrapper
  {
  private:
    class ParamRef
    {
    public:
      virtual bool try_update(const rclcpp::ParameterValue& new_val) = 0;
    };

    template <typename T>
    class ParamRefImpl : public ParamRef
    {
    private:
      T& m_ref;

    public:
      ParamRefImpl(T& ref_to) : m_ref(ref_to){};

      bool try_update(const rclcpp::ParameterValue& new_val) override
      {
        try
        {
          m_ref = new_val.get<T>();
          return true;
        }
        catch (const rclcpp::ParameterTypeException&)
        {
          return false;
        }
      }
    };

  private:
    std::unique_ptr<ParamRef> m_pimpl;
    const std::string m_rclcpp_type_name;

  public:
    template <typename T>
    ParamRefWrapper(T& ref_to, const rclcpp::ParameterType rclcpp_type) :
      m_pimpl(std::make_unique<ParamRefImpl<T>>(ref_to)),
      m_rclcpp_type_name(rclcpp::to_string(rclcpp_type))
    {};

    bool try_update(const rclcpp::ParameterValue& new_val)
    {
      assert(m_pimpl);
      return m_pimpl->try_update(new_val);
    }

    std::string get_type_name()
    {
      return m_rclcpp_type_name;
    }
  };

  //}

  /*** ParamLoader CLASS //{ **/

  /**
   * \brief Convenience class for loading parameters from rosparam server.
   *
   * The parameters can be loaded as compulsory. If a compulsory parameter is not found
   * on the rosparam server (e.g. because it is missing in the launchfile or the yaml config file),
   * an internal flag is set to false, indicating that the parameter loading procedure failed.
   * This flag can be checked using the loaded_successfully() method after all parameters were
   * attempted to be loaded (see usage example usage below).
   *
   * The loaded parameter names and corresponding values are printed to stdout by default
   * for user convenience. Special cases such as loading of Eigen matrices or loading
   * of std::vectors of various values are also provided.
   *
   */
  class ParamLoader
  {

  private:
    enum unique_t
    {
      UNIQUE = true,
      REUSABLE = false
    };
    enum optional_t
    {
      OPTIONAL = true,
      COMPULSORY = false
    };
    enum swap_t
    {
      SWAP = true,
      NO_SWAP = false
    };

  private:
    bool m_load_successful, m_print_values;
    rclcpp::Node& m_nh;
    rclcpp::Logger m_logger;
    rclcpp::node_interfaces::OnSetParametersCallbackHandle::SharedPtr m_param_callback_handle;
    std::unordered_set<std::string> m_loaded_params;
    std::unordered_map<std::string, ParamRefWrapper> m_dynamic_params;

    /* printing helper functions //{ */
    /* print_error and print_warning functions //{*/
    void print_error(const std::string str)
    {
      RCLCPP_ERROR(m_logger, "%s", str.c_str());
    }
    void print_warning(const std::string str)
    {
      RCLCPP_WARN(m_logger, "%s", str.c_str());
    }
    //}

    /* print_param function and overloads //{ */

    template <typename T>
    void print_param(const std::string& name, const T& val)
    {
      RCLCPP_INFO_STREAM(m_logger, "parameter '" << name << "':\t" << val);
    };

    //}

    std::string resolved(const std::string& param_name)
    {
      return param_name;
    }
    //}

    /* params_callback() method //{ */

    rcl_interfaces::msg::SetParametersResult params_callback(std::vector<rclcpp::Parameter> params)
    {
      bool success = true;
      for (const auto& param : params)
      {
        const auto name = param.get_name();
        auto dynparam_it = m_dynamic_params.find(name);
        // skip if the parameter was not marked as dynamic
        if (dynparam_it == std::end(m_dynamic_params))
        {
          print_warning("Tried to dynamically set an unknown parameter: \"" + name + "\" with value " + param.value_to_string() + ".");
          continue;
        }

        // if the program flow gets here, we know this parameter is a dynamic parameter
        ParamRefWrapper& prw = dynparam_it->second;
        if (!prw.try_update(param.get_parameter_value()))
        {
          print_error("Tried to dynamically set an incompatible parameter: \"" + name + "\" with a wrong type " + param.get_type_name()
                      + ". Expected type: " + prw.get_type_name() + ". The value attempted to be set is " + param.value_to_string() + "). Ignoring.");
          success = false;
        }
      }

      if (success)
      {
        rcl_interfaces::msg::SetParametersResult result;
        result.successful = true;
        result.reason = "all went fine and dandy";
        return result;
      } else
      {
        rcl_interfaces::msg::SetParametersResult result;
        result.successful = false;
        result.reason = "type mismatch in some parameters";
        return result;
      }
    }

    //}

    /* check_duplicit_loading checks whether the parameter was already loaded - returns true if yes //{ */
    bool check_duplicit_loading(const std::string& name)
    {
      if (m_loaded_params.count(name))
      {
        print_error(std::string("Tried to load parameter ") + name + std::string(" twice"));
        m_load_successful = false;
        return true;
      } else
      {
        return false;
      }
    }
    //}

    /* load helper function for generic types //{ */
    // This function tries to load a parameter with name 'name' and a default value.
    // You can use the flag 'optional' to not throw a ROS_ERROR when the parameter
    // cannot be loaded and the flag 'printValues' to set whether the loaded
    // value and name of the parameter should be printed to cout.
    // If 'optional' is set to false and the parameter could not be loaded,
    // the flag 'load_successful' is set to false and a ROS_ERROR message
    // is printer.
    // If 'unique' flag is set to false then the parameter is not checked
    // for being loaded twice.
    // Returns a tuple, containing either the loaded or the default value and a bool,
    // indicating if the value was loaded (true) or the default value was used (false).
    template <typename T>
    std::tuple<T, rclcpp::ParameterType, bool> load(const std::string& name, const T& default_value, optional_t optional = OPTIONAL, unique_t unique = UNIQUE)
    {
      const rclcpp::ParameterType type = rclcpp::ParameterValue(default_value).get_type();
      if (unique && check_duplicit_loading(name))
        return {default_value, type, false};

      bool default_used = false;
      bool load_successful = true;
      // declare the parameter
      m_nh.declare_parameter(name);
      // try to load the parameter
      rclcpp::Parameter param;
      const bool param_exists = m_nh.get_parameter(name, param);

      T loaded_val;
      bool loaded_convertible_type = false;
      if (param_exists)
      {
        // try to convert the loaded parameter to the desired type
        try
        {
          loaded_val = param.get_value<T>();
          loaded_convertible_type = true;
        } catch (const rclcpp::ParameterTypeException&) {};
      }

      if (!loaded_convertible_type)
      {
        // if it was not loaded, set the default value
        loaded_val = default_value;
        default_used = true;
        if (!optional)
        {
          // if the parameter was compulsory, alert the user and set the flag
          print_error(std::string("Could not load non-optional parameter ") + resolved(name));
          load_successful = false;
        }
      }

      if (load_successful)
      {
        // everything is fine and just print the name and value if required
        if (m_print_values)
          print_param(name, loaded_val);
        // mark the param name as successfully loaded
        m_loaded_params.insert(name);
      } else
      {
        m_load_successful = false;
      }
      // finally, return the resulting value
      return {loaded_val, type, default_used};
    };
    //}

  public:
    /* Constructor //{ */

    /*!
     * \brief Main constructor.
     *
     * \param nh            The parameters will be loaded from rosparam using this node handle.
     * \param printValues   If true, the loaded values will be printed to stdout.
     */
    ParamLoader(rclcpp::Node& nh, bool printValues = true) : m_load_successful(true), m_print_values(printValues), m_nh(nh), m_logger(m_nh.get_logger())
    {
      m_param_callback_handle = nh.add_on_set_parameters_callback(std::bind(&ParamLoader::params_callback, this, std::placeholders::_1));
    };

    //}

    /* loaded_successfully() method //{ */
    /*!
     * \brief Indicates whether all compulsory parameters were successfully loaded.
     *
     * \return false if any compulsory parameter was not loaded (is not present at rosparam server). Otherwise returns true.
     */
    bool loaded_successfully()
    {
      return m_load_successful;
    };
    //}

    /* load_param function for optional parameters //{ */
    /*!
     * \brief Loads a parameter from the rosparam server with a default value.
     *
     * If the parameter with the specified name is not found on the rosparam server (e.g. because it is not specified in the launchfile or yaml config file),
     * the default value is used.
     * Using this method, the parameter can only be loaded once using the same ParamLoader instance without error.
     *
     * \param name          Name of the parameter in the rosparam server.
     * \param out_value     Reference to the variable to which the parameter value will be stored (such as a class member variable).
     * \param default_value This value will be used if the parameter name is not found in the rosparam server.
     * \param dynamic       Whether the variable referenced by \p out_value should be automatically modified during dynamic reconfigure callbacks.
     * \return              true if the parameter was loaded from \p rosparam, false if the default value was used.
     */
    template <typename T>
    bool load_param(const std::string& name, T& out_value, const T& default_value, bool dynamic = false)
    {
      const auto [ret_val, ret_type, from_rosparam] = load<T>(name, default_value, OPTIONAL, UNIQUE);
      out_value = ret_val;
      if (dynamic)
      {
        ParamRefWrapper wrp(out_value, ret_type);
        m_dynamic_params.emplace(std::make_pair(name, std::move(wrp)));
      }
      return from_rosparam;
    };
    /*!
     * \brief Loads a parameter from the rosparam server with a default value.
     *
     * If the parameter with the specified name is not found on the rosparam server (e.g. because it is not specified in the launchfile or yaml config file),
     * the default value is used.
     * Using this method, the parameter can only be loaded once using the same ParamLoader instance without error.
     *
     * \param name          Name of the parameter in the rosparam server.
     * \param default_value This value will be used if the parameter name is not found in the rosparam server.
     * \return              The loaded parameter value.
     */
    template <typename T>
    T load_param2(const std::string& name, const T& default_value)
    {
      const auto loaded = load<T>(name, default_value, OPTIONAL, UNIQUE);
      return std::get<0>(loaded);
    };
    /*!
     * \brief Loads a parameter from the rosparam server with a default value.
     *
     * If the parameter with the specified name is not found on the rosparam server (e.g. because it is not specified in the launchfile or yaml config file),
     * the default value is used.
     * Using this method, the parameter can be loaded multiple times using the same ParamLoader instance without error.
     *
     * \param name          Name of the parameter in the rosparam server.
     * \param default_value This value will be used if the parameter name is not found in the rosparam server.
     * \return              The loaded parameter value.
     */
    template <typename T>
    T load_param_reusable(const std::string& name, const T& default_value)
    {
      const auto loaded = load<T>(name, default_value, OPTIONAL, REUSABLE);
      return std::get<0>(loaded);
    };
    //}

    /* load_param function for compulsory parameters //{ */
    /*!
     * \brief Loads a compulsory parameter from the rosparam server.
     *
     * If the parameter with the specified name is not found on the rosparam server (e.g. because it is not specified in the launchfile or yaml config file),
     * the loading process is unsuccessful (loaded_successfully() will return false).
     * Using this method, the parameter can only be loaded once using the same ParamLoader instance without error.
     *
     * \param name          Name of the parameter in the rosparam server.
     * \param out_value     Reference to the variable to which the parameter value will be stored (such as a class member variable).
     * \param dynamic       Whether the variable referenced by \p out_value should be automatically modified during dynamic reconfigure callbacks.
     * \return              true if the parameter was loaded from \p rosparam, false if the default value was used.
     */
    template <typename T>
    bool load_param(const std::string& name, T& out_value, [[maybe_unused]] bool dynamic = false)
    {
      const auto [ret, ret_type, from_rosparam] = load<T>(name, T(), COMPULSORY, UNIQUE);
      out_value = ret;
      /* if (dynamic) */
      /* { */
      /*   ParamRefWrapper wrp(out_value, ret_type); */
      /*   m_dynamic_params.emplace(std::make_pair(name, std::move(wrp))); */
      /* } */
      return from_rosparam;
    };
    /*!
     * \brief Loads a compulsory parameter from the rosparam server.
     *
     * If the parameter with the specified name is not found on the rosparam server (e.g. because it is not specified in the launchfile or yaml config file),
     * the loading process is unsuccessful (loaded_successfully() will return false).
     * Using this method, the parameter can only be loaded once using the same ParamLoader instance without error.
     *
     * \param name          Name of the parameter in the rosparam server.
     * \return              The loaded parameter value.
     */
    template <typename T>
    T load_param2(const std::string& name)
    {
      const auto loaded = load<T>(name, T(), COMPULSORY, UNIQUE);
      return std::get<0>(loaded);
    };

    /*!
     * \brief Loads a compulsory parameter from the rosparam server.
     *
     * If the parameter with the specified name is not found on the rosparam server (e.g. because it is not specified in the launchfile or yaml config file),
     * the loading process is unsuccessful (loaded_successfully() will return false).
     * Using this method, the parameter can be loaded multiple times using the same ParamLoader instance without error.
     *
     * \param name          Name of the parameter in the rosparam server.
     * \return              The loaded parameter value.
     */
    template <typename T>
    T load_param_reusable(const std::string& name)
    {
      const auto loaded = load<T>(name, T(), COMPULSORY, REUSABLE);
      return std::get<0>(loaded);
    };
    //}
  };
  //}

}  // namespace mrs_lib

#endif  // PARAM_LOADER_H
