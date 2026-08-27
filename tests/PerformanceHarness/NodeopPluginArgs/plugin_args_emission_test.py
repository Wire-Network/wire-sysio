#!/usr/bin/env python3

"""Guards how BasePluginArgs renders nodeop command-line options.

Arity is a property of what nodeop declares, not of the field's Python type: an option can be typed
bool and still consume a value, and a switch can be typed str. Rendering the wrong shape makes
nodeop exit with invalid_bool_value or silently swallow the next argument, so each shape is pinned
here.
"""

from dataclasses import dataclass

from BasePluginArgs import BasePluginArgs


@dataclass
class SampleArgs(BasePluginArgs):
    _pluginNamespace: str = "sysio"
    _pluginName: str = "sample_plugin"

    # bool_switch(): takes no value. Typed str by the generator before arity was recorded.
    finalityDataHistory: bool = None
    _finalityDataHistoryNodeopDefault: bool = False
    _finalityDataHistoryNodeopArg: str = "--finality-data-history"
    _finalityDataHistoryNodeopArgTakesValue: bool = False

    # value<bool>()->default_value(1): takes a value, which must render as 1/0 rather than True/False.
    disableSubjectiveP2pBilling: int = None
    _disableSubjectiveP2pBillingNodeopDefault: int = 1
    _disableSubjectiveP2pBillingNodeopArg: str = "--disable-subjective-p2p-billing"
    _disableSubjectiveP2pBillingNodeopArgTakesValue: bool = True

    # value<vector<string>>()->multitoken(): takes a value and has no default. Typed bool by the
    # generator's old name heuristic because the option name contains "disable".
    disableSubjectiveAccountBilling: str = None
    _disableSubjectiveAccountBillingNodeopDefault: str = None
    _disableSubjectiveAccountBillingNodeopArg: str = "--disable-subjective-account-billing"
    _disableSubjectiveAccountBillingNodeopArgTakesValue: bool = True

    # value<uint32_t>(): ordinary non-boolean value option.
    subjectiveAccountMaxFailures: int = None
    _subjectiveAccountMaxFailuresNodeopDefault: int = 3
    _subjectiveAccountMaxFailuresNodeopArg: str = "--subjective-account-max-failures"
    _subjectiveAccountMaxFailuresNodeopArgTakesValue: bool = True


def rendered(**kwargs) -> str:
    return str(SampleArgs(**kwargs))


def test_defaults_emit_nothing():
    assert rendered() == ""


def test_switch_emits_bare_flag():
    assert rendered(finalityDataHistory=True) == \
        "--plugin sysio::sample_plugin --finality-data-history"


def test_boolean_valued_option_emits_one_or_zero():
    assert rendered(disableSubjectiveP2pBilling=0) == \
        "--plugin sysio::sample_plugin --disable-subjective-p2p-billing 0"
    # A Python bool assigned to a value-taking option must still render as 1/0, not True/False.
    assert rendered(disableSubjectiveP2pBilling=False) == \
        "--plugin sysio::sample_plugin --disable-subjective-p2p-billing 0"


def test_value_option_typed_bool_still_takes_its_value():
    assert rendered(disableSubjectiveAccountBilling="account1") == \
        "--plugin sysio::sample_plugin --disable-subjective-account-billing account1"


def test_non_boolean_value_option():
    assert rendered(subjectiveAccountMaxFailures=5) == \
        "--plugin sysio::sample_plugin --subjective-account-max-failures 5"


def test_supported_args_excludes_arity_metadata():
    assert SampleArgs().supportedNodeopArgs() == [
        "--finality-data-history",
        "--disable-subjective-p2p-billing",
        "--disable-subjective-account-billing",
        "--subjective-account-max-failures",
    ]


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            fn()
            print(f"{name}: ok")
    print("*** No errors detected")


if __name__ == '__main__':
    main()
