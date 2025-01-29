#pragma once

#include <sysio/chain/name.hpp>
#include <sysio/chain/asset.hpp>
#include <fc/reflect/reflect.hpp>

namespace sysio {
   class roa {
      public:
         // Exactly match the layout from the WASM side:
         struct nodeowners {
            sysio::chain::name   owner;          
            uint8_t              tier;        
            sysio::chain::asset  total_sys;     
            sysio::chain::asset  allocated_sys; 
            sysio::chain::asset  allocated_bw;  
            sysio::chain::asset  allocated_ram; 
            uint8_t              network_gen;   
         };

         struct policies {
            sysio::chain::name   owner;        
            sysio::chain::name   issuer;       
            sysio::chain::asset  net_weight;  
            sysio::chain::asset  cpu_weight;  
            sysio::chain::asset  ram_weight;  
            uint64_t             bytes_per_unit;
            uint32_t             time_block;   
         };

         struct reslimit {
            sysio::chain::name   owner;       
            sysio::chain::asset  net_weight; 
            sysio::chain::asset  cpu_weight; 
            uint64_t             ram_bytes;   
         };
   }; // namespace roa
} // namespace sysio

FC_REFLECT(sysio::roa::nodeowners, (owner)(tier)(total_sys)(allocated_sys)(allocated_bw)(allocated_ram)(network_gen))
FC_REFLECT(sysio::roa::policies, (owner)(issuer)(net_weight)(cpu_weight)(ram_weight)(bytes_per_unit)(time_block))
FC_REFLECT(sysio::roa::reslimit, (owner)(net_weight)(cpu_weight)(ram_bytes))
