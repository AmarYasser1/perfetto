// Copyright (C) 2023 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import m from 'mithril';

import {classNames} from '../base/classnames';

import {HTMLCheckboxAttrs} from './common';

export interface CheckboxAttrs extends HTMLCheckboxAttrs {
  // Optional text to show to the right of the checkbox.
  label?: string;
}

export class Checkbox implements m.ClassComponent<CheckboxAttrs> {
  view({attrs}: m.CVnode<CheckboxAttrs>) {
    const {label, disabled, checked, className, ...htmlAttrs} = attrs;
    const classes = classNames(
        disabled && 'pf-disabled',
        className,
    );

    // The default checkbox is removed and an entirely new one created inside
    // the span element in CSS.
    return m(
        'label.pf-checkbox',
        {
          ...htmlAttrs,
          className: classes,
        },
        m('input[type=checkbox]', {disabled, checked}),
        m('span'),
        label,
    );
  }
}
