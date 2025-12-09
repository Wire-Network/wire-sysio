# Signature Provider Manager Plugin

This plugin is the result of refactoring the
existing `signature_provider_plugin`.

The goal of the refactoring it to support multiple 
signature providers targeting different chains,
with varied key types.

## Provider Spec

```
<name>,<chain-kind>,<key-type>,<public-key>,<private-key-provider-spec>

   `<name>`                 name to use when referencing this provider, if empty then auto-assigned
   `<chain-kind>`           chain where the key will be used
   `<key-type>`             key format to parse
   `<public-key>`           is a string form of a valid <key-type>
   `<key-provider-spec>`        is a string in the form <key-provider-type>:<data>
       
```
## Key Provider Spec

```
<key-provider-type>:<data>
   `<key-provider-type>`    is KEY, KIOD, or SE
   `<data>`                is provided to the key provider based on the type
        `<private-key>`    string representation of a key in the format of the key type for `KEY` provider type
        `<url>`            is the URL where kiod is available and the appropriate wallet(s) for `KIOD` provider type
```


## Examples

### WIRE

```
# Private key: 5J5LzjfChtY3LGhkxaRoAaSjKHtgNZqKyaJaw5boxuY9LNv4e1U
# Public key: SYS7AzqPxqfoEigXBefEo6efsCZszLzwv4vCdWqTt6s6zSnDELSmm
wire-01,wire,wire,SYS7AzqPxqfoEigXBefEo6efsCZszLzwv4vCdWqTt6s6zSnDELSmm,KEY:5J5LzjfChtY3LGhkxaRoAaSjKHtgNZqKyaJaw5boxuY9LNv4e1U
```

### Ethereum

```
# PRIVATE_KEY: 0x8f2cdaeb8e036865421c79d4cc42c7704af5cef0f592b2e5c993e2ba7d328248
# PUBLIC_KEY: 0xfc5422471c9e31a6cd6632a2858eeaab39f9a7eec5f48eedecf53b8398521af1c86c9fce17312900cbb11e2e2ec1fb706598065f855c2f8f2067e1fbc1ba54c8
eth-01,ethereum,ethereum,0xfc5422471c9e31a6cd6632a2858eeaab39f9a7eec5f48eedecf53b8398521af1c86c9fce17312900cbb11e2e2ec1fb706598065f855c2f8f2067e1fbc1ba54c8,KEY:0x8f2cdaeb8e036865421c79d4cc42c7704af5cef0f592b2e5c993e2ba7d328248
```