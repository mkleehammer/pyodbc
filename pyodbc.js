
$(function() {
  $('a[href=#toggle-show]').on('click', function(e) {
    console.log('click');

    e.preventDefault();
    e.stopPropagation();

    var n = $(e.target).closest('.toggle-top')
        .find('.toggle-show').toggleClass('hide');
  });
});
