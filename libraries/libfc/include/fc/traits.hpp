#pragma once

#include <string>
#include <boost/type_index.hpp>

namespace fc {
  template <typename T>
  struct pretty_type {
    std::string name() {
      // auto& info = boost::typeindex::type_id_with_cvr<T>().type_info();
      // auto id = boost::typeindex::type_id_with_cvr<T>();
      // std::string name = id.name();
      // return name;
      auto name = boost::core::demangle(typeid(this).name());
      auto offset = name.rfind("::");
      if (offset != std::string::npos)
         name.erase(0, offset+2);
      name = name.substr(0, name.find('>'));
      return name;
    };
  };

}
