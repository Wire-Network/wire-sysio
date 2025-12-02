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